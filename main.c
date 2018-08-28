// $Id: main.c,v 1.117 2018/08/26 18:16:49 karn Exp $
// Read complex float samples from multicast stream (e.g., from funcube.c)
// downconvert, filter, demodulate, optionally compress and multicast audio
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

#define MAXPKT 1500 // Maximum bytes of data in incoming I/Q packet

void closedown(int);
void *rtp_recv(void *);

// Primary control blocks for downconvert/filter/demodulate and audio output
// Note: initialized to all zeroes, like all global variables
struct demod Demod;
struct audio Audio;

// Parameters with default values
char Libdir[] = "/usr/local/share/ka9q-radio";
int Nthreads = 1;
int DAC_samprate = 48000;
int Quiet = 0;
int Verbose = 0;
char Statepath[PATH_MAX];
char Locale[256] = "en_US.UTF-8";
int Update_interval = 100;  // 100 ms between screen updates
int SDR_correct = 0;
uint32_t Ssrc = 0;

void audio_cleanup(void *);

void cleanup(void){
  audio_cleanup(&Audio);  // Not really necessary
}

void closedown(int a){
  if(!Quiet)
    fprintf(stderr,"Signal %d\n",a);
  exit(1);
}


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

  atexit(cleanup);

  if(readmodes("modes.txt") != 0){
    fprintf(stderr,"Can't read mode table\n");
    exit(1);
  }

  // Must do this before first filter is created with set_mode(), otherwise a segfault can occur
  fftwf_import_system_wisdom();
  fftwf_make_planner_thread_safe();

  struct demod * const demod = &Demod; // Only one demodulator per program for now
  struct audio * const audio = &Audio;

  // Set program defaults, can be overridden by state file and command line args, in that order
  memset(demod,0,sizeof(*demod)); // Just in case it's ever dynamic
  audio->samprate = DAC_samprate; // currently 48 kHz, hard to change
  strcpy(demod->mode,"FM");
  demod->freq = 147.435e6;  // LA "animal house" repeater, active all night for testing

  demod->L = 3840;      // Number of samples in buffer: FFT length = L + M - 1
  demod->M = 4352+1;    // Length of filter impulse response
  demod->kaiser_beta = 3.0; // Reasonable compromise
  strlcpy(demod->iq_mcast_address_text,"iq.hf.mcast.local",sizeof(demod->iq_mcast_address_text));
  demod->headroom = pow(10.,-15./20); // -15 dB
  strlcpy(audio->audio_mcast_address_text,"pcm.hf.mcast.local",sizeof(audio->audio_mcast_address_text));
  demod->tunestep = 0;  // single digit hertz position
  demod->imbalance = 1; // 0 dB

  // set invalid to start
  demod->input_source_address.sa_family = -1; // Set invalid
  demod->low = NAN;
  demod->high = NAN;
  set_shift(demod,0);

  // Find any file argument and load it
  char optstring[] = "cd:f:I:k:l:L:m:M:r:R:qs:t:T:u:vS:";
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
    case 'c':
      SDR_correct = 1;
      break;
    case 'd':
      demod->doppler_command = optarg;
      break;
    case 'f':   // Initial RF tuning frequency
      demod->freq = parse_frequency(optarg);
      break;
    case 'I':   // Multicast address to listen to for I/Q data
      strlcpy(demod->iq_mcast_address_text,optarg,sizeof(demod->iq_mcast_address_text));
      break;
    case 'k':   // Kaiser window shape parameter; 0 = rectangular
      demod->kaiser_beta = strtod(optarg,NULL);
      break;
    case 'l':   // Locale, mainly for numerical output format
      strlcpy(Locale,optarg,sizeof(Locale));
      setlocale(LC_ALL,Locale);
      break;
    case 'L':   // Pre-detection filter block size
      demod->L = strtol(optarg,NULL,0);
      break;
    case 'm':   // receiver mode (AM/FM, etc)
      strlcpy(demod->mode,optarg,sizeof(demod->mode));
      break;
    case 'M':   // Pre-detection filter impulse length
      demod->M = strtol(optarg,NULL,0);
      break;
    case 'q':
      Quiet++;  // Suppress display
      break;
    case 'R':   // Set audio target IP multicast address
      strlcpy(Audio.audio_mcast_address_text,optarg,sizeof(Audio.audio_mcast_address_text));
      break;
    case 's':
      {
	double shift = strtod(optarg,NULL);
	set_shift(demod,shift);
      }
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
      Ssrc = strtol(optarg,NULL,0);
      break;
    default:
      fprintf(stderr,"Usage: %s [-d doppler_command] [-f frequency] [-I iq multicast address] [-k kaiser_beta] [-l locale] [-L blocksize] [-m mode] [-M FIRlength] [-q] [-R Audio multicast address] [-s shift offset] [-t threads] [-u update_ms] [-v]\n",argv[0]);
      exit(1);
      break;
    }
  }
  fprintf(stderr,"General coverage receiver for the Funcube Pro and Pro+\n");
  fprintf(stderr,"Copyright 2017 by Phil Karn, KA9Q; may be used under the terms of the GNU General Public License\n");
  
  // Set up actual demod state
  demod->ctl_fd = -1;   // Invalid
  demod->input_fd = -1; // Invalid

#if !defined(NDEBUG)
  // Detect early starts
  demod->second_LO_phasor = NAN;
  demod->second_LO_phasor_step = NAN;
#endif

  pthread_mutex_init(&demod->status_mutex,NULL);
  pthread_cond_init(&demod->status_cond,NULL);
  pthread_mutex_init(&demod->doppler_mutex,NULL);
  pthread_mutex_init(&demod->shift_mutex,NULL);
  pthread_mutex_init(&demod->second_LO_mutex,NULL);
  pthread_mutex_init(&demod->qmutex,NULL);
  pthread_cond_init(&demod->qcond,NULL);

  // Input socket for I/Q data from SDR
  demod->input_fd = setup_mcast(demod->iq_mcast_address_text,0);
  if(demod->input_fd == -1){
    fprintf(stderr,"Can't set up I/Q input\n");
    exit(1);
  }
  // For sending commands to front end
  if((demod->ctl_fd = socket(PF_INET,SOCK_DGRAM, 0)) == -1)
    perror("can't open control socket");

  // Blocksize really should be computed from demod->L and decimate
  if(setup_audio(audio) != 0){
    fprintf(stderr,"Audio setup failed\n");
    exit(1);
  }
  // Create master half of filter
  // Must be done before the demodulator starts or it will fail an assert
  // If done in proc_samples(), will be a race condition
  demod->filter_in = create_filter_input(demod->L,demod->M,COMPLEX);

  pthread_create(&demod->rtp_recv_thread,NULL,rtp_recv,demod);
  pthread_create(&demod->proc_samples,NULL,proc_samples,demod);

  // Optional doppler correction
  if(demod->doppler_command)
    pthread_create(&demod->doppler_thread,NULL,doppler,demod);

  // Actually set the mode and frequency already specified
  // These wait until the SDR sample rate is known, so they'll block if the SDR isn't running
  fprintf(stderr,"Waiting for SDR response...\n");
  set_freq(demod,demod->freq,NAN); 
  demod->gain = dB2voltage(100.0); // Empirical starting value
  set_mode(demod,demod->mode,0); // Don't override with defaults from mode table 

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  // Become the display thread unless quiet; then just twiddle our thumbs
  pthread_t display_thread;
  if(!Quiet){
    pthread_create(&display_thread,NULL,display,demod);
  }
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

    socklen_t socksize = sizeof(demod->input_source_address);
    int size = recvfrom(demod->input_fd,pkt->content,sizeof(pkt->content),0,&demod->input_source_address,&socksize);
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
  
    // Note these are in host byte order, i.e., *little* endian because we don't have to interoperate with anything else
    struct status new_status;
    new_status.timestamp = *(long long *)dp;
    new_status.frequency = *(double *)&dp[8];
    new_status.samprate = *(uint32_t *)&dp[16];
    new_status.lna_gain = dp[20];
    new_status.mixer_gain = dp[21];
    new_status.if_gain = dp[22];
    dp += 24;
    size -= 24;
    update_status(demod,&new_status);

    pkt->data = dp;
    pkt->len = size;

    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;
    pthread_mutex_lock(&demod->qmutex);
    for(qe = demod->queue; qe && pkt->rtp.seq >= qe->rtp.seq; q_prev = qe,qe = qe->next)
      ;

    pkt->next = qe;
    if(q_prev)
      q_prev->next = pkt;
    else
      demod->queue = pkt; // Front of list

    pkt = NULL;        // force new packet to be allocated
    // wake up decoder thread
    pthread_cond_signal(&demod->qcond);
    pthread_mutex_unlock(&demod->qmutex);
  }      
  return NULL;
}


// Save receiver state to file
// Path is Statepath[] = $HOME/.radiostate
int savestate(struct demod *dp,char const *filename){
  // Dump receiver state to file
  struct audio *audio = &Audio; // Eventually make parameter

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
  fprintf(fp,"Source %s\n",dp->iq_mcast_address_text);
  if(audio){
    fprintf(fp,"Audio output %s\n",audio->audio_mcast_address_text);
    fprintf(fp,"TTL %d\n",Mcast_ttl);
  }
  fprintf(fp,"Blocksize %d\n",dp->L);
  fprintf(fp,"Impulse len %d\n",dp->M);
  fprintf(fp,"Frequency %.3f Hz\n",get_freq(dp));
  fprintf(fp,"Mode %s\n",dp->mode);
  fprintf(fp,"Shift %.3f Hz\n",dp->shift);
  fprintf(fp,"Filter low %.3f Hz\n",dp->low);
  fprintf(fp,"Filter high %.3f Hz\n",dp->high);
  fprintf(fp,"Tunestep %d\n",dp->tunestep);
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
  struct audio *audio = &Audio; // Eventually make parameter

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
    if(sscanf(line,"Frequency %lf",&dp->freq) > 0){
    } else if(strncmp(line,"Mode ",5) == 0){
      strlcpy(dp->mode,&line[5],sizeof(dp->mode));
    } else if(sscanf(line,"Shift %lf",&dp->shift) > 0){
    } else if(sscanf(line,"Filter low %f",&dp->low) > 0){
    } else if(sscanf(line,"Filter high %f",&dp->high) > 0){
    } else if(sscanf(line,"Kaiser Beta %f",&dp->kaiser_beta) > 0){
    } else if(sscanf(line,"Blocksize %d",&dp->L) > 0){
    } else if(sscanf(line,"Impulse len %d",&dp->M) > 0){
    } else if(sscanf(line,"Tunestep %d",&dp->tunestep) > 0){
    } else if(sscanf(line,"Source %256s",dp->iq_mcast_address_text) > 0){
      // Array sizes defined elsewhere!
    } else if(audio && sscanf(line,"Audio output %256s",audio->audio_mcast_address_text) > 0){
    } else if(sscanf(line,"TTL %d",&Mcast_ttl) > 0){
    } else if(sscanf(line,"Locale %256s",Locale)){
      setlocale(LC_ALL,Locale);
    }
  }
  fclose(fp);
  return 0;
}
