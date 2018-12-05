// $Id: main.c,v 1.127 2018/12/05 07:08:41 karn Exp $
// Read complex float samples from multicast stream (e.g., from funcube.c)
// downconvert, filter, demodulate, optionally compress and multicast output
// Copyright 2017, Phil Karn, KA9Q, karn@ka9q.net
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>

#include "misc.h"
#include "dsp.h"
#include "multicast.h"
#include "radio.h"
#include "filter.h"
#include "status.h"


// Config constants
#define MAXPKT 1500 // Maximum bytes of data in incoming I/Q packet
char Libdir[] = "/usr/local/share/ka9q-radio";
int static DAC_samprate = 48000;

// Command line Parameters with default values
int Nthreads = 1;
int Quiet = 0;
int Verbose = 0;
char Statepath[PATH_MAX];
char Locale[256] = "en_US.UTF-8";
int Update_interval = 100;  // 100 ms between screen updates
int Mcast_ttl = 1;

// Primary control blocks for downconvert/filter/demodulate and output
// Note: initialized to all zeroes, like all global variables
struct demod Demod;

struct timeval Starttime;      // System clock at timestamp 0, for RTCP

extern uint64_t Commands;

void output_cleanup(void *);
void closedown(int);
void *rtp_recv(void *);
void *rtcp_send(void *);
void cleanup(void);
void closedown(int);
void *recv_sdr_status(void *);
void *send_status(void *);

// The main program sets up the demodulator parameter defaults,
// overwrites them with command-line arguments and/or state file settings,
// initializes the various local oscillators, pthread mutexes and conditions
// sets up multicast I/Q input and PCM audio output
// Sets up the input half of the pre-detection filter
// starts the RTP input and downconverter/filter threads
// sets the initial demodulation mode, which starts the demodulator thread
// catches signals and eventually becomes the user interface/display loop
int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");

  // Set up program defaults
  // Some can be overridden by state file or command line args
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  snprintf(Statepath,sizeof(Statepath),"%s/%s",getenv("HOME"),".radiostate");
  Statepath[sizeof(Statepath)-1] = '\0';

  if(readmodes("modes.txt") != 0){
    fprintf(stderr,"Can't read mode table\n");
    exit(1);
  }

  // Must do this before first filter is created with set_mode(), otherwise a segfault can occur
  fftwf_import_system_wisdom();
  fftwf_make_planner_thread_safe();

  struct demod * const demod = &Demod; // Only one demodulator per program for now

  // Set program defaults, can be overridden by state file and command line args, in that order
  memset(demod,0,sizeof(*demod)); // Just in case it's ever dynamic
  demod->output.samprate = DAC_samprate; // currently 48 kHz, hard to change
  strcpy(demod->mode,"FM");
  demod->tune.freq = 147.435e6;  // LA "animal house" repeater, active all night for testing

  demod->filter.L = 3840;      // Number of samples in buffer: FFT length = L + M - 1
  demod->filter.M = 4352+1;    // Length of filter impulse response
  demod->filter.kaiser_beta = 3.0; // Reasonable compromise
  strlcpy(demod->input.dest_address_text,"iq.hf.mcast.local",sizeof(demod->input.dest_address_text));
  demod->agc.headroom = pow(10.,-15./20); // -15 dB
  strlcpy(demod->output.dest_address_text,"pcm.hf.mcast.local",sizeof(demod->output.dest_address_text));
  demod->tune.step = 0;  // single digit hertz position
  demod->tune.shift = NAN;
  demod->sdr.imbalance = 1; // 0 dB
  demod->filter.decimate = 1; // default to avoid division by zero
  demod->filter.interpolate = 1;

  // set invalid to start
  demod->input.source_address.ss_family = -1; // Set invalid
  demod->filter.low = NAN;
  demod->filter.high = NAN;

  // Find any file argument and load it
  char optstring[] = "d:f:I:k:l:L:m:M:r:R:qs:t:T:u:vS:";
  while(getopt(argc,argv,optstring) != -1)
    ;
  if(argc > optind)
    loadstate(demod,argv[optind]);
  else
    loadstate(demod,"default");
  
  // Go back and re-read args for real this time, possibly overwriting loaded parameters
  optind = 1;
  int c;
  while((c = getopt(argc,argv,optstring)) != EOF){
    switch(c){
    case 'd':
      demod->doppler_command = optarg;
      break;
    case 'f':   // Initial RF tuning frequency
      demod->tune.freq = parse_frequency(optarg);
      break;
    case 'I':   // Multicast address to listen to for I/Q data
      strlcpy(demod->input.dest_address_text,optarg,sizeof(demod->input.dest_address_text));
      break;
    case 'k':   // Kaiser window shape parameter; 0 = rectangular
      demod->filter.kaiser_beta = strtod(optarg,NULL);
      break;
    case 'l':   // Locale, mainly for numerical output format
      strlcpy(Locale,optarg,sizeof(Locale));
      setlocale(LC_ALL,Locale);
      break;
    case 'L':   // Pre-detection filter block size
      demod->filter.L = strtol(optarg,NULL,0);
      break;
    case 'm':   // receiver mode (AM/FM, etc)
      strlcpy(demod->mode,optarg,sizeof(demod->mode));
      break;
    case 'M':   // Pre-detection filter impulse length
      demod->filter.M = strtol(optarg,NULL,0);
      break;
    case 'q':
      Quiet++;  // Suppress display
      break;
    case 'R':   // Set output target IP multicast address
      strlcpy(demod->output.dest_address_text,optarg,sizeof(demod->output.dest_address_text));
      break;
    case 's':
      demod->tune.shift = strtod(optarg,NULL);
      break;
    case 'T': // TTL on output packets
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 't':   // # of threads to use in FFTW3
      Nthreads = strtol(optarg,NULL,0);
      fftwf_init_threads();
      fftwf_plan_with_nthreads(Nthreads);
      fprintf(stderr,"Using %d threads for FFTs\n",Nthreads);
      break;
    case 'u':   // Display update rate
      Update_interval = strtol(optarg,NULL,0);
      break;
    case 'v':   // Extra debugging
      Verbose++;
      break;
    case 'S':   // Set SSRC on output stream
      demod->output.rtp.ssrc = strtol(optarg,NULL,0);
      break;
    default:
      fprintf(stderr,"Usage: %s [-d doppler_command] [-f frequency] [-I iq multicast address] [-k kaiser_beta] [-l locale] [-L blocksize] [-m mode] [-M FIRlength] [-q] [-R Output multicast address] [-s shift offset] [-t threads] [-u update_ms] [-v]\n",argv[0]);
      exit(1);
      break;
    }
  }
  fprintf(stderr,"General coverage receiver for the Funcube Pro and Pro+\n");
  fprintf(stderr,"Copyright 2017 by Phil Karn, KA9Q; may be used under the terms of the GNU General Public License\n");
  
  pthread_mutex_init(&demod->sdr.status_mutex,NULL);
  pthread_cond_init(&demod->sdr.status_cond,NULL);
  pthread_mutex_init(&demod->doppler.mutex,NULL);
  pthread_mutex_init(&demod->shift.mutex,NULL);
  pthread_mutex_init(&demod->second_LO.mutex,NULL);
  pthread_mutex_init(&demod->input.qmutex,NULL);
  pthread_cond_init(&demod->input.qcond,NULL);

  // Input socket for I/Q data from SDR
  demod->input.fd = setup_mcast(demod->input.dest_address_text,(struct sockaddr *)&demod->input.dest_address,0,0,0);
  if(demod->input.fd == -1){
    fprintf(stderr,"Can't set up I/Q input\n");
    exit(1);
  }
  // Output socket for commands to SDR
  demod->input.ctl_fd = setup_mcast(demod->input.dest_address_text,NULL,1,Mcast_ttl,2);

  gettimeofday(&Starttime,NULL);

  if(setup_output(demod,Mcast_ttl) != 0){
    fprintf(stderr,"Output setup failed\n");
    exit(1);
  }
  // Create master half of filter
  // Must be done before the demodulator starts or it will fail an assert
  // If done in proc_samples(), will be a race condition
  // Blocksize really should be computed from demod->filter.L and decimate
  demod->filter.in = create_filter_input(demod->filter.L,demod->filter.M,COMPLEX);

  pthread_t rtp_recv_thread,proc_samples_thread;
  pthread_create(&rtp_recv_thread,NULL,rtp_recv,demod);
  pthread_create(&proc_samples_thread,NULL,proc_samples,demod);

  // Optional doppler correction
  if(demod->doppler_command)
    pthread_create(&demod->doppler_thread,NULL,doppler,demod);

  pthread_t status_thread;
  pthread_create(&status_thread,NULL,send_status,demod);

  pthread_t rtcp_thread;
  pthread_create(&rtcp_thread,NULL,rtcp_send,demod);

  pthread_t recv_sdr_status_thread;
  pthread_create(&recv_sdr_status_thread,NULL,recv_sdr_status,demod);


  // Block until we get a packet from the SDR and we know the sample rate
  fprintf(stderr,"Waiting for first SDR packet to learn sample rate..."); fflush(stderr);
  pthread_mutex_lock(&demod->sdr.status_mutex);
  while(demod->sdr.status.samprate == 0)
    pthread_cond_wait(&demod->sdr.status_cond,&demod->sdr.status_mutex);
  pthread_mutex_unlock(&demod->sdr.status_mutex);
  fprintf(stderr,"%'d Hz\n",demod->sdr.status.samprate);

  //  sleep(2);
  // Actually set the mode and frequency already specified
  set_mode(demod,demod->mode,0); // Don't override with defaults from mode table 

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  // Start the display thread unless quiet; then just twiddle our thumbs
  pthread_t display_thread;
  if(!Quiet)
    pthread_create(&display_thread,NULL,display,demod);

  while(1)
    usleep(1000000); // probably get rid of this

  exit(0);
}


// Thread to read from RTP network socket, remove DC offsets,
// fix I/Q gain and phase imbalance,
// Write corrected data to circular buffer, wake up demodulator thread(s)
// when data is available and when SDR status (frequency, sampling rate) changes
void *rtp_recv(void *arg){
  pthread_setname("rtp-rcv");
  assert(arg != NULL);
  struct demod * const demod = arg;
  
  struct packet *pkt = NULL;

  while(1){
    // Packet consists of Ethernet, IP and UDP header (already stripped)
    // then standard Real Time Protocol (RTP), a status header and the PCM
    // I/Q data. RTP is an IETF standard, so it uses big endian numbers
    // The status header and I/Q data are *not* standard, so we save time
    // by using machine byte order (almost certainly little endian).
    // Note this is a portability problem if this system and the one generating
    // the data have opposite byte orders. But who's big endian anymore?
    // Receive I/Q data from front end
    // Incoming RTP packets

    if(!pkt)
      pkt = malloc(sizeof(*pkt));

    socklen_t socksize = sizeof(demod->input.source_address);
    int size = recvfrom(demod->input.fd,pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&demod->input.source_address,&socksize);
    if(size <= 0){    // ??
      perror("recvfrom");
      usleep(50000);
      continue;
    }
    if(size < RTP_MIN_SIZE)
      continue; // Too small for RTP, ignore

    unsigned char *dp = pkt->content;
    dp = ntoh_rtp(&pkt->rtp,dp);
    size -= (dp - pkt->content);
    
    if(pkt->rtp.pad){
      // Remove padding
      size -= dp[size-1];
      pkt->rtp.pad = 0;
    }
    if(pkt->rtp.type != IQ_PT && pkt->rtp.type != IQ_PT8)
      continue; // Wrong type
  
    // Unlike 'opus', 'packet', 'monitor', etc, 'radio' is nominally an interactive program so we don't keep track of SSRCs.
    // All digital IF traffic to this multicast group with the correct payload type will be accepted so that we don't have to restart
    // the single copy of 'radio' when the SDR hardware daemon restarts.
    // Maybe this should change, but it's hard to know what to do except when running as a non-interactive
    // background daemon. In that case, a new SSRC in the digital IF stream could fork a new instance of 'radio' with the same parameters,
    // and it in turn would send demodulated PCM to a new SSRC.

    // Old status information, now obsolete, replaced by TLV streams on port 5006. Ignore for now, eventually it'll go away entirely
    // These are in host byte order, i.e., *little* endian because we don't have to interoperate with anything else
    dp += 24;
    size -= 24;

    pkt->data = dp;
    pkt->len = size;

    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;
    pthread_mutex_lock(&demod->input.qmutex);
    for(qe = demod->input.queue; qe && pkt->rtp.seq >= qe->rtp.seq; q_prev = qe,qe = qe->next)
      ;

    pkt->next = qe;
    if(q_prev)
      q_prev->next = pkt;
    else
      demod->input.queue = pkt; // Front of list

    pkt = NULL;        // force new packet to be allocated
    // wake up decoder thread
    pthread_cond_signal(&demod->input.qcond);
    pthread_mutex_unlock(&demod->input.qmutex);
  }      
  return NULL;
}


// Save receiver state to file
// Path is Statepath[] = $HOME/.radiostate
int savestate(struct demod *dp,char const *filename){
  FILE *fp;
  char pathname[PATH_MAX];
  if(filename[0] == '/')
    strlcpy(pathname,filename,sizeof(pathname));    // Absolute path
  else
    snprintf(pathname,sizeof(pathname),"%s/%s",Statepath,filename);

  if((fp = fopen(pathname,"w")) == NULL){
    fprintf(stderr,"Can't write state file %s\n",pathname);
    return -1;
  }
  fprintf(fp,"#KA9Q DSP Receiver State dump\n");
  fprintf(fp,"Locale %s\n",Locale);
  fprintf(fp,"Source %s\n",dp->input.dest_address_text);
  fprintf(fp,"Output %s\n",dp->output.dest_address_text);
  fprintf(fp,"TTL %d\n",Mcast_ttl);
  fprintf(fp,"Blocksize %d\n",dp->filter.L);
  fprintf(fp,"Impulse len %d\n",dp->filter.M);
  fprintf(fp,"Frequency %.3f Hz\n",dp->tune.freq);
  fprintf(fp,"Mode %s\n",dp->mode);
  fprintf(fp,"Shift %.3f Hz\n",dp->tune.shift);
  fprintf(fp,"Filter low %.3f Hz\n",dp->filter.low);
  fprintf(fp,"Filter high %.3f Hz\n",dp->filter.high);
  fprintf(fp,"Tunestep %d\n",dp->tune.step);
  fclose(fp);
  return 0;
}
// Load receiver state from file
// Some of these are problematic since they're overwritten from the mode
// table when the mode is actually set on the first A/D packet:
// shift, filter low, filter high, tuning step (not currently set)
// 
int loadstate(struct demod *dp,char const *filename){
  FILE *fp;

  char pathname[PATH_MAX];
  if(filename[0] == '/')
    strlcpy(pathname,filename,sizeof(pathname));
  else
    snprintf(pathname,sizeof(pathname),"%s/%s",Statepath,filename);

  if((fp = fopen(pathname,"r")) == NULL){
    fprintf(stderr,"Can't read state file %s\n",pathname);
    return -1;
  }
  char line[PATH_MAX];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    if(sscanf(line,"Frequency %lf",&dp->tune.freq) > 0){
    } else if(strncmp(line,"Mode ",5) == 0){
      strlcpy(dp->mode,&line[5],sizeof(dp->mode));
    } else if(sscanf(line,"Shift %lf",&dp->tune.shift) > 0){
    } else if(sscanf(line,"Filter low %f",&dp->filter.low) > 0){
    } else if(sscanf(line,"Filter high %f",&dp->filter.high) > 0){
    } else if(sscanf(line,"Kaiser Beta %f",&dp->filter.kaiser_beta) > 0){
    } else if(sscanf(line,"Blocksize %d",&dp->filter.L) > 0){
    } else if(sscanf(line,"Impulse len %d",&dp->filter.M) > 0){
    } else if(sscanf(line,"Tunestep %d",&dp->tune.step) > 0){
    } else if(sscanf(line,"Source %256s",dp->input.dest_address_text) > 0){
      // Array sizes defined elsewhere!
    } else if(sscanf(line,"Output %256s",dp->output.dest_address_text) > 0){
    } else if(sscanf(line,"TTL %d",&Mcast_ttl) > 0){
    } else if(sscanf(line,"Locale %256s",Locale)){
      setlocale(LC_ALL,Locale);
    }
  }
  fclose(fp);
  return 0;
}

// RTP control protocol sender task
void *rtcp_send(void *arg){
  struct demod *demod = (struct demod *)arg;
  if(demod == NULL)
    pthread_exit(NULL);

  pthread_setname("rtcp");
  //  fprintf(stderr,"hello from rtcp_send\n");
  while(1){

    if(demod->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    unsigned char buffer[4096]; // much larger than necessary
    memset(buffer,0,sizeof(buffer));
    
    // Construct sender report
    struct rtcp_sr sr;
    memset(&sr,0,sizeof(sr));
    sr.ssrc = demod->output.rtp.ssrc;

    // Construct NTP timestamp
    struct timeval tv;
    gettimeofday(&tv,NULL);
    double runtime = (tv.tv_sec - Starttime.tv_sec) + (tv.tv_usec - Starttime.tv_usec)/1000000.;

    long long now_time = ((long long)tv.tv_sec + NTP_EPOCH)<< 32;
    now_time += ((long long)tv.tv_usec << 32) / 1000000;

    sr.ntp_timestamp = now_time;
    // The zero is to remind me that I start timestamps at zero, but they could start anywhere
    sr.rtp_timestamp = 0 + runtime * 48000;
    sr.packet_count = demod->output.rtp.seq;
    sr.byte_count = demod->output.rtp.bytes;
    
    unsigned char *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

    // Construct SDES
    struct rtcp_sdes sdes[4];
    
    // CNAME
    char hostname[1024];
    gethostname(hostname,sizeof(hostname));
    char *string = NULL;
    int sl = asprintf(&string,"radio@%s",hostname);
    if(sl > 0 && sl <= 255){
      sdes[0].type = CNAME;
      strcpy(sdes[0].message,string);
      sdes[0].mlen = strlen(sdes[0].message);
    }
    if(string){
      free(string); string = NULL;
    }

    sdes[1].type = NAME;
    strcpy(sdes[1].message,"KA9Q Radio Program");
    sdes[1].mlen = strlen(sdes[1].message);
    
    sdes[2].type = EMAIL;
    strcpy(sdes[2].message,"karn@ka9q.net");
    sdes[2].mlen = strlen(sdes[2].message);

    sdes[3].type = TOOL;
    strcpy(sdes[3].message,"KA9Q Radio Program");
    sdes[3].mlen = strlen(sdes[3].message);
    
    dp = gen_sdes(dp,sizeof(buffer) - (dp-buffer),demod->output.rtp.ssrc,sdes,4);


    send(demod->output.rtcp_fd,buffer,dp-buffer,0);
  done:;
    usleep(1000000);
  }
}
void closedown(int a){
  if(!Quiet)
    fprintf(stderr,"Signal %d\n",a);
  exit(1);
}
