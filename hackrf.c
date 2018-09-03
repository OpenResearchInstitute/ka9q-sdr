// $Id: hackrf.c,v 1.11 2018/08/29 01:34:15 karn Exp $
// Read from HackRF
// Multicast raw 8-bit I/Q samples
// Accept control commands from UDP socket
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <libhackrf/hackrf.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>

#include "sdr.h"
#include "radio.h"
#include "misc.h"
#include "multicast.h"
#include "decimate.h"


struct sdrstate {
  hackrf_device *device;
  struct status status;     // Frequency and gain settings, grouped for transmission in RTP packet

  float in_power;              // Running estimate of unfiltered A/D signal power
  float out_power;             // Filtered output power
  
  int clips;                // Sample clips since last reset

  // Smoothed error estimates
  complex float DC;      // DC offset
  float sinphi;          // I/Q phase error
  float imbalance;       // Ratio of I power to Q power
};


// Configurable parameters
// decibel limits for power
float Upper_limit = -15;
float Lower_limit = -25;

int ADC_samprate; // Computed from Out_samprate * Decimate
int Out_samprate = 192000;
int Decimate = 64;
int Log_decimate = 6; // Computed from Decimate
float Filter_atten = 1;
int Blocksize = 350;
int Device = 0;      // Which of several to use
int Offset=1;     // Default to offset high by +Fs/4 downconvert in software to avoid DC
int Daemonize = 0;

char *Rundir = "/run/hackrf";


float const DC_alpha = 1.0e-7;  // high pass filter coefficient for DC offset estimates, per sample
float const Power_alpha= 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates
int const stage_threshold = 8; // point at which to switch to filter f8
#define BUFFERSIZE  (1<<19) // Upcalls seem to be 256KB; don't make too big or we may blow out of the cache
const float SCALE8 = 1./127.;   // Scale 8-bit samples to unity range floats


struct sdrstate HackCD;
char *Locale;
pthread_t Display_thread;
pthread_t Process_thread;
pthread_t AGC_thread;
int Rtp_sock; // Socket handle for sending real time stream *and* receiving commands
int Ctl_sock;
extern int Mcast_ttl;
uint32_t Ssrc;
int Seq = 0;
int Timestamp = 0;

complex float Sampbuffer[BUFFERSIZE];
int Samp_wp;
int Samp_rp;
FILE *Status;
char *Status_filename;
char *Pid_filename;

pthread_mutex_t Buf_mutex;
pthread_cond_t Buf_cond;

void *display(void *arg);
void *agc(void *arg);
double  rffc5071_freq(uint16_t lo);
uint32_t max2837_freq(uint32_t freq);


void errmsg(const char *fmt,...){
  va_list ap;

  va_start(ap,fmt);

  if(Daemonize){
    vsyslog(LOG_INFO,fmt,ap);
  } else {
    vfprintf(stderr,fmt,ap);
    fflush(stderr);
  }
  va_end(ap);
}


// Gain and phase corrections. These will be updated every block
float gain_q = 1;
float gain_i = 1;
float secphi = 1;
float tanphi = 0;

// Callback called with incoming receiver data from A/D
int rx_callback(hackrf_transfer *transfer){

  int remain = transfer->valid_length; // Count of individual samples; divide by 2 to get complex samples
  int samples = remain / 2;            // Complex samples
  unsigned char *dp = transfer->buffer;

  complex float samp_sum = 0;
  float i_energy=0,q_energy=0;
  float dotprod = 0;                           // sum of I*Q, for phase balance
  float rate_factor = 1./(ADC_samprate * Power_alpha);

  while(remain > 0){
    complex float samp;
    int isamp_i = (char)*dp++;
    int isamp_q = (char)*dp++;
    remain -= 2;

    if(isamp_q == -128){
      HackCD.clips++;
      isamp_q = -127;
    }
    if(isamp_i == -128){
      HackCD.clips++;
      isamp_i = -127;
    }
    samp = CMPLXF(isamp_i,isamp_q) * SCALE8; // -1.0 to +1.0

    samp_sum += samp;

    // remove DC offset (which can be fractional)
    samp -= HackCD.DC;
    
    // Must correct gain and phase before frequency shift
    // accumulate I and Q energies before gain correction
    i_energy += crealf(samp) * crealf(samp);
    q_energy += cimagf(samp) * cimagf(samp);
    
    // Balance gains, keeping constant total energy
    __real__ samp *= gain_i;
    __imag__ samp *= gain_q;
    
    // Accumulate phase error
    dotprod += crealf(samp) * cimagf(samp);

    // Correct phase
    __imag__ samp = secphi * cimagf(samp) - tanphi * crealf(samp);
    
    Sampbuffer[Samp_wp] = samp;
    Samp_wp = (Samp_wp + 1) & (BUFFERSIZE-1);
  }
  pthread_cond_signal(&Buf_cond); // Wake him up only after we're done
  // Update every block
  // estimates of DC offset, signal powers and phase error
  HackCD.DC += DC_alpha * (samp_sum - samples*HackCD.DC);
  float block_energy = 0.5 * (i_energy + q_energy); // Normalize for complex pairs
  if(block_energy > 0){ // Avoid divisions by 0, etc
    //HackCD.in_power += rate_factor * (block_energy - samples*HackCD.in_power); // Average A/D output power per channel  
    HackCD.in_power = block_energy/samples; // Average A/D output power per channel  
    HackCD.imbalance += rate_factor * samples * ((i_energy / q_energy) - HackCD.imbalance);
    float dpn = dotprod / block_energy;
    HackCD.sinphi += rate_factor  * samples * (dpn - HackCD.sinphi);
    gain_q = sqrtf(0.5 * (1 + HackCD.imbalance));
    gain_i = sqrtf(0.5 * (1 + 1./HackCD.imbalance));
    secphi = 1/sqrtf(1 - HackCD.sinphi * HackCD.sinphi); // sec(phi) = 1/cos(phi)
    tanphi = HackCD.sinphi * secphi;                     // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
  }
  return 0;
}

void *process(void *arg){

  pthread_setname("hackrf-proc");

  unsigned char buffer[200+2*Blocksize*sizeof(short)];
  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = IQ_PT;
  rtp.ssrc = Ssrc;
  int rotate_phase = 0;

  // Decimation filter states
  struct hb15_state hb15_state_real[Log_decimate];
  struct hb15_state hb15_state_imag[Log_decimate];
  memset(hb15_state_real,0,sizeof(hb15_state_real));
  memset(hb15_state_imag,0,sizeof(hb15_state_imag));
  float hb3state_real[Log_decimate];
  float hb3state_imag[Log_decimate];
  memset(hb3state_real,0,sizeof(hb3state_real));
  memset(hb3state_imag,0,sizeof(hb3state_imag));

#if 0
  // Verify SIMD alignment
  assert(((uint64_t)hb15_state_real & 15) == 0);
  assert(((uint64_t)hb15_state_imag & 15) == 0);
#endif

  // Initialize coefficients here!!!
  // As experiment, use Goodman/Carey "F8" 15-tap filter
  // Note word order in array -- [3] is closest to the center, [0] is on the tails
  for(int i=0; i<Log_decimate; i++){ // For each stage (h(0) is always unity, other h(n) are zero for even n)
    hb15_state_real[i].coeffs[3] = 490./802;
    hb15_state_imag[i].coeffs[3] = 490./802;
    hb15_state_real[i].coeffs[2] = -116./802;
    hb15_state_imag[i].coeffs[2] = -116./802;
    hb15_state_real[i].coeffs[1] = 33./802; 
    hb15_state_imag[i].coeffs[1] = 33./802;    
    hb15_state_real[i].coeffs[0] = -6./802; 
    hb15_state_imag[i].coeffs[0] = -6./802;    
  }
  float time_p_packet = (float)Blocksize / Out_samprate;
  while(1){

    rtp.timestamp = Timestamp;
    rtp.seq = Seq++;

    unsigned char *dp = buffer;
    dp = hton_rtp(dp,&rtp);
    dp = hton_status(dp,&HackCD.status);

    // Wait for enough to be available
    pthread_mutex_lock(&Buf_mutex);
    while(1){
      int avail = (Samp_wp - Samp_rp) & (BUFFERSIZE-1);
      if(avail >= Blocksize*Decimate)
	break;
      pthread_cond_wait(&Buf_cond,&Buf_mutex);
    }
    pthread_mutex_unlock(&Buf_mutex);
    
    float workblock_real[Decimate*Blocksize];    // Hold input to first decimator, half used on each filter call
    float workblock_imag[Decimate*Blocksize];

    // Load first stage with corrected samples
    int loop_limit = Decimate * Blocksize;
    for(int i=0; i<loop_limit; i++){
      complex float samp = Sampbuffer[Samp_rp++];
      float samp_i = crealf(samp);
      float samp_q = cimagf(samp);
      Samp_rp &= (BUFFERSIZE-1); // Assume even buffer size

      // Increase frequency by Fs/4 to compensate for tuner being high by Fs/4
      switch(rotate_phase){
      default:
      case 0:
	workblock_real[i] = samp_i;
	workblock_imag[i] = samp_q;
	break;
      case 1:
	workblock_real[i] = -samp_q;
	workblock_imag[i] = samp_i;
	break;
      case 2:
	workblock_real[i] = -samp_i;
	workblock_imag[i] = -samp_q;
	break;
      case 3:
	workblock_real[i] = samp_q;
	workblock_imag[i] = -samp_i;
	break;
      }
      rotate_phase += Offset;
      rotate_phase &= 3; // Modulo 4
    }

    // Real channel decimation
    // First stages can use simple, fast filter; later ones use slower filter
    int j;
    for(j=Log_decimate-1;j>=stage_threshold;j--)
      hb3_block(&hb3state_real[j],workblock_real,workblock_real,(1<<j)*Blocksize);

    for(; j>=0;j--)
      hb15_block(&hb15_state_real[j],workblock_real,workblock_real,(1<<j)*Blocksize);

    float output_energy = 0;
    signed short *up = (signed short *)dp;
    loop_limit = Blocksize;
    for(int j=0;j<loop_limit;j++){
      float s = workblock_real[j] * Filter_atten;
      output_energy += s*s;
      *up++ = (short)round(32767 * s);
      up++;
    }

    // Imaginary channel decimation
    for(j=Log_decimate-1;j>=stage_threshold;j--)
      hb3_block(&hb3state_imag[j],workblock_imag,workblock_imag,(1<<j)*Blocksize);

    for(; j>=0;j--)
      hb15_block(&hb15_state_imag[j],workblock_imag,workblock_imag,(1<<j)*Blocksize);

    // Interleave imaginary samples following real
    up = (signed short *)dp;
    loop_limit = Blocksize;
    for(int j=0;j<loop_limit;j++){
      float s = workblock_imag[j] * Filter_atten;
      output_energy += s*s;
      up++;
      *up++ = (short)round(32767 * s);
    }

    HackCD.out_power = 0.5 * output_energy / Blocksize;
    dp = (unsigned char *)up;
    if(send(Rtp_sock,buffer,dp - buffer,0) == -1){
      errmsg("send: %s",strerror(errno));
      // If we're sending to a unicast address without a listener, we'll get ECONNREFUSED
      // Sleep 1 sec to slow down the rate of these messages
      usleep(1000000);
    }
    Timestamp += Blocksize; // samples
  
    // Simply increment by number of samples
    // But what if we lose some? Then the clock will always be off
    HackCD.status.timestamp += 1.e9 * time_p_packet;

  }
}


int main(int argc,char *argv[]){
#if 0 // Better handled in systemd?
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    errmsg("seteuid: %s",strerror(errno));
#endif

  char *dest = "239.1.6.1"; // Default for testing

  Locale = getenv("LANG");
  if(Locale == NULL || strlen(Locale) == 0)
    Locale = "en_US.UTF-8";

  int c;
  while((c = getopt(argc,argv,"D:I:dvl:b:R:T:o:r:S:")) != -1){
    switch(c){
    case 'd':
      Daemonize++;
      Status = NULL;
      break;
    case 'o':
      Offset = strtol(optarg,NULL,0);
      break;
    case 'r':
      Out_samprate = strtol(optarg,NULL,0);
      break;
    case 'R':
      dest = optarg;
      break;
    case 'D':
      Decimate = strtol(optarg,NULL,0);
      break;
    case 'I':
      Device = strtol(optarg,NULL,0);
      break;
    case 'v':
      if(!Daemonize)
	Status = stderr;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'b':
      Blocksize = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'S':
      Ssrc = strtol(optarg,NULL,0);
      break;
    default:
    case '?':
      fprintf(stderr,"Unknown argument %c\n",c);
      break;
    }
  }
  if(Daemonize){
    // I know this is deprecated, replace it someday with posix_spawn()
    if(daemon(0,0) != 0)
      exit(1);

    openlog("hackrf",LOG_PID,LOG_DAEMON);

#if 0 // Now handled by systemd
    mkdir(Rundir,0775); // Ensure it exists, let everybody read it
#endif
    
    // see if one is already running
    int r = asprintf(&Pid_filename,"%s%d/pid",Rundir,Device);
    if(r == -1){
      // Unlikely, but it makes the compiler happy
      errmsg("asprintf error");
      exit(1);
    }
    FILE *pidfile = fopen(Pid_filename,"r");
    if(pidfile){
      // pid file exists; read it and see if process exists
      int pid = 0;
      if(fscanf(pidfile,"%d",&pid) == 1 && (kill(pid,0) == 0 || errno != ESRCH)){
	// Already running; exit
	fclose(pidfile);
	errmsg("pid %d: daemon %d already running, quitting",getpid(),pid);
	exit(1);
      }
      fclose(pidfile); pidfile = NULL;
    }
    unlink(Pid_filename); // Remove any orphan
    pidfile = fopen(Pid_filename,"w");
    if(pidfile){
      int pid = getpid();
      fprintf(pidfile,"%d\n",pid);
      fclose(pidfile);
    }
    r = asprintf(&Status_filename,"%s%d/status",Rundir,Device);
    if(r == -1){
      // Unlikely, but it makes the compiler happy
      errmsg("asprintf error");
      exit(1);
    }

    unlink(Status_filename); // Remove any orphaned version
    Status = fopen(Status_filename,"w");
    if(Status == NULL){
      errmsg("Can't write %s: %s\n",Status_filename,strerror(errno));
    } else {
      setlinebuf(Status);
    }
  } else {
    Status = stderr; // Write status to stderr when running in foreground
  }
  
  ADC_samprate = Decimate * Out_samprate;
  Log_decimate = (int)round(log2(Decimate));
  if(1<<Log_decimate != Decimate){
    errmsg("Decimation ratios must currently be a power of 2\n");
    exit(1);
  }
  Filter_atten = powf(.5, Log_decimate); // Compensate for +6dB gain in each decimation stage

  setlocale(LC_ALL,Locale);
  
  // Set up RTP output socket
  Rtp_sock = setup_mcast(dest,1);
  if(Rtp_sock == -1){
    errmsg("Can't create multicast socket: %s",strerror(errno));
    exit(1);
  }
    
  // Set up control socket
  Ctl_sock = socket(AF_INET,SOCK_DGRAM,0);

  // bind control socket to next sequential port after our multicast source port
  struct sockaddr_in ctl_sockaddr;
  socklen_t siz = sizeof(ctl_sockaddr);
  if(getsockname(Rtp_sock,(struct sockaddr *)&ctl_sockaddr,&siz) == -1){
    errmsg("getsockname on ctl port: %s",strerror(errno));
    exit(1);
  }
  struct sockaddr_in locsock;
  locsock.sin_family = AF_INET;
  locsock.sin_port = htons(ntohs(ctl_sockaddr.sin_port)+1);
  locsock.sin_addr.s_addr = INADDR_ANY;
  bind(Ctl_sock,(struct sockaddr *)&locsock,sizeof(locsock));

  int ret;
  if((ret = hackrf_init()) != HACKRF_SUCCESS){
    errmsg("hackrf_init() failed: %s\n",hackrf_error_name(ret));
    exit(1);
  }
  // Enumerate devices
  hackrf_device_list_t *dlist = hackrf_device_list();

  if((ret = hackrf_device_list_open(dlist,Device,&HackCD.device)) != HACKRF_SUCCESS){
    errmsg("hackrf_open(%d) failed: %s\n",Device,hackrf_error_name(ret));
    exit(1);
  }
  hackrf_device_list_free(dlist); dlist = NULL;

  ret = hackrf_set_sample_rate(HackCD.device,(double)ADC_samprate);
  assert(ret == HACKRF_SUCCESS);
  HackCD.status.samprate = Out_samprate;

  uint32_t bw = hackrf_compute_baseband_filter_bw_round_down_lt(ADC_samprate);
  ret = hackrf_set_baseband_filter_bandwidth(HackCD.device,bw);
  assert(ret == HACKRF_SUCCESS);

  // NOTE: what we call mixer gain, they call lna gain
  // What we call lna gain, they call antenna enable
  HackCD.status.lna_gain = 14;
  HackCD.status.mixer_gain = 24;
  HackCD.status.if_gain = 20;

  ret = hackrf_set_antenna_enable(HackCD.device,HackCD.status.lna_gain ? 1 : 0);
  assert(ret == HACKRF_SUCCESS);
  ret = hackrf_set_lna_gain(HackCD.device,HackCD.status.mixer_gain);
  assert(ret == HACKRF_SUCCESS);
  ret = hackrf_set_vga_gain(HackCD.device,HackCD.status.if_gain);
  assert(ret == HACKRF_SUCCESS);

  uint64_t intfreq = HackCD.status.frequency = 146000000;
  
  intfreq += Offset * ADC_samprate / 4; // Offset tune high by +Fs/4

  ret = hackrf_set_freq(HackCD.device,intfreq);
  assert(ret == HACKRF_SUCCESS);

  pthread_mutex_init(&Buf_mutex,NULL);
  pthread_cond_init(&Buf_cond,NULL);


  time_t tt;
  time(&tt);
  struct timeval tp;
  gettimeofday(&tp,NULL);
  // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
  HackCD.status.timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;

  if(Ssrc == 0)
    Ssrc = tt & 0xffffffff; // low 32 bits of clock time
  errmsg("uid %d; device %d; dest %s; blocksize %d; RTP SSRC %lx; status file %s\n",getuid(),Device,dest,Blocksize,Ssrc,Status_filename);
  errmsg("A/D sample rate %'d Hz; decimation ratio %d; output sample rate %'d Hz; Offset %'+d\n",
	 ADC_samprate,Decimate,Out_samprate,Offset * ADC_samprate/4);

  pthread_create(&Process_thread,NULL,process,NULL);

  ret = hackrf_start_rx(HackCD.device,rx_callback,&HackCD);
  assert(ret == HACKRF_SUCCESS);

  pthread_create(&AGC_thread,NULL,agc,NULL);

  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  
  if(Status)
    pthread_create(&Display_thread,NULL,display,NULL);


  // Process commands to change hackrf state
  // We listen on the same IP address and port we use as a multicasting source
  pthread_setname("hackrf-cmd");

  while(1){

    fd_set fdset;
    socklen_t addrlen;
    int ret;
    struct timeval timeout;
    struct status requested_status;
    
    // Read with a timeout - necessary?
    FD_ZERO(&fdset);
    FD_SET(Ctl_sock,&fdset);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    ret = select(Ctl_sock+1,&fdset,NULL,NULL,&timeout);
    if(ret == -1){
      errmsg("select");
      usleep(50000); // don't loop tightly
      continue;
    }
    if(ret == 0)
      continue;

    // A command arrived, read it
    // Should probably log these
    struct sockaddr_in6 command_address;
    addrlen = sizeof(command_address);
    if((ret = recvfrom(Ctl_sock,&requested_status,sizeof(requested_status),0,(struct sockaddr *)&command_address,&addrlen)) <= 0){
      if(ret < 0)
	errmsg("recv");
      usleep(50000); // don't loop tightly
      continue;
    }
    
    if(ret < sizeof(requested_status))
      continue; // Too short; ignore
    
    uint64_t intfreq = HackCD.status.frequency = requested_status.frequency;
    intfreq += Offset * ADC_samprate/4; // Offset tune by +Fs/4

    ret = hackrf_set_freq(HackCD.device,intfreq);
    assert(ret == HACKRF_SUCCESS);
  }
  // Can't really get here
  close(Rtp_sock);
  hackrf_close(HackCD.device);
  hackrf_exit();
  exit(0);
}



// Status display thread
void *display(void *arg){
  pthread_setname("hackrf-disp");

  fprintf(Status,"               |---Gains dB---|      |----Levels dB --|   |---------Errors---------|           clips\n");
  fprintf(Status,"Frequency      LNA  mixer bband          RF   A/D   Out     DC-I   DC-Q  phase  gain\n");
  fprintf(Status,"Hz                                           dBFS  dBFS                    deg    dB\n");   

  off_t stat_point = ftello(Status);
  // End lines with return when writing to terminal, newlines when writing to status file
  char   eol = stat_point == -1 ? '\r' : '\n';
  while(1){

    float powerdB = 10*log10f(HackCD.in_power);

    if(stat_point != -1)
      fseeko(Status,stat_point,SEEK_SET);
    
    fprintf(Status,"%'-15.0lf%3d%7d%6d%'12.1f%'6.1f%'6.1f%9.4f%7.4f%7.2f%6.2f%'16d    %c",
	    HackCD.status.frequency,
	    HackCD.status.lna_gain,	    
	    HackCD.status.mixer_gain,
	    HackCD.status.if_gain,
	    powerdB - (HackCD.status.lna_gain + HackCD.status.mixer_gain + HackCD.status.if_gain),
	    powerdB,
	    10*log10f(HackCD.out_power),
	    crealf(HackCD.DC),
	    cimagf(HackCD.DC),
	    (180/M_PI) * asin(HackCD.sinphi),
	    10*log10(HackCD.imbalance),
	    HackCD.clips,
	    eol);
    fflush(Status);
    usleep(100000); // 10 Hz
  }
  return NULL;
}

void *agc(void *arg){
  while(1){
    usleep(100000);
    float powerdB = 10*log10f(HackCD.in_power);
    int change;
    if(powerdB > Upper_limit)
      change = Upper_limit - powerdB;
    else if(powerdB < Lower_limit)
      change = Lower_limit - powerdB;
    else
      continue;
    
    int ret __attribute__((unused)) = HACKRF_SUCCESS; // Won't be used when asserts are disabled
    if(change > 0){
      // Increase gain, LNA first, then mixer, and finally IF
      if(change >= 14 && HackCD.status.lna_gain < 14){
	HackCD.status.lna_gain = 14;
	change -= 14;
	ret = hackrf_set_antenna_enable(HackCD.device,HackCD.status.lna_gain ? 1 : 0);
	assert(ret == HACKRF_SUCCESS);
      }
      int old_mixer_gain = HackCD.status.mixer_gain;
      int new_mixer_gain = min(40,old_mixer_gain + 8*(change/8));
      if(new_mixer_gain != old_mixer_gain){
	HackCD.status.mixer_gain = new_mixer_gain;
	change -= new_mixer_gain - old_mixer_gain;
	ret = hackrf_set_lna_gain(HackCD.device,HackCD.status.mixer_gain);
	assert(ret == HACKRF_SUCCESS);
      }
      int old_if_gain = HackCD.status.if_gain;
      int new_if_gain = min(62,old_if_gain + 2*(change/2));
      if(new_if_gain != old_if_gain){
	HackCD.status.if_gain = new_if_gain;
	change -= new_if_gain - old_if_gain;
	ret = hackrf_set_vga_gain(HackCD.device,HackCD.status.if_gain);
	assert(ret == HACKRF_SUCCESS);
      }
    } else if(change < 0){
      // Reduce gain (IF first), start counter
      int old_if_gain = HackCD.status.if_gain;
      int new_if_gain = max(0,old_if_gain + 2*(change/2));
      if(new_if_gain != old_if_gain){
	HackCD.status.if_gain = new_if_gain;
	change -= new_if_gain - old_if_gain;
	ret = hackrf_set_vga_gain(HackCD.device,HackCD.status.if_gain);
	assert(ret == HACKRF_SUCCESS);
      }
      int old_mixer_gain = HackCD.status.mixer_gain;
      int new_mixer_gain = max(0,old_mixer_gain + 8*(change/8));
      if(new_mixer_gain != old_mixer_gain){
	HackCD.status.mixer_gain = new_mixer_gain;
	change -= new_mixer_gain - old_mixer_gain;
	ret = hackrf_set_lna_gain(HackCD.device,HackCD.status.mixer_gain);
	assert(ret == HACKRF_SUCCESS);
      }
      int old_lna_gain = HackCD.status.lna_gain;
      int new_lna_gain = max(0,old_lna_gain + 14*(change/14));
      if(new_lna_gain != old_lna_gain){
	HackCD.status.lna_gain = new_lna_gain;
	change -= new_lna_gain - old_lna_gain;
	ret = hackrf_set_antenna_enable(HackCD.device,HackCD.status.lna_gain ? 1 : 0);
	assert(ret == HACKRF_SUCCESS);
      }
    }
  }
}

void closedown(int a){
  errmsg("caught signal %d: %s\n",a,strsignal(a));
  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    exit(0);
  exit(1);
}

// extracted from hackRF firmware/common/rffc5071.c
// Used to set RFFC5071 upconverter to multiples of 1 MHz
// for future use in determining exact tuning frequency

#define LO_MAX 5400.0
#define REF_FREQ 50.0
#define FREQ_ONE_MHZ (1000.0*1000.0)

double  rffc5071_freq(uint16_t lo) {
	uint8_t lodiv;
	uint16_t fvco;
	uint8_t fbkdiv;
	
	/* Calculate n_lo */
	uint8_t n_lo = 0;
	uint16_t x = LO_MAX / lo;
	while ((x > 1) && (n_lo < 5)) {
		n_lo++;
		x >>= 1;
	}

	lodiv = 1 << n_lo;
	fvco = lodiv * lo;

	if (fvco > 3200) {
		fbkdiv = 4;
	} else {
		fbkdiv = 2;
	}

	uint64_t tmp_n = ((uint64_t)fvco << 29ULL) / (fbkdiv*REF_FREQ) ;

	return (REF_FREQ * (tmp_n >> 5ULL) * fbkdiv * FREQ_ONE_MHZ)
			/ (lodiv * (1 << 24ULL));
}
uint32_t max2837_freq(uint32_t freq){
	uint32_t div_frac;
	//	uint32_t div_int;
	uint32_t div_rem;
	uint32_t div_cmp;
	int i;

	/* ASSUME 40MHz PLL. Ratio = F*(4/3)/40,000,000 = F/30,000,000 */
	//	div_int = freq / 30000000;
       	div_rem = freq % 30000000;
	div_frac = 0;
	div_cmp = 30000000;
	for( i = 0; i < 20; i++) {
		div_frac <<= 1;
		div_cmp >>= 1;
		if (div_rem > div_cmp) {
			div_frac |= 0x1;
			div_rem -= div_cmp;
		}
	}
	return div_rem;

}
#if 0

#define FREQ_ONE_MHZ     (1000*1000)

#define MIN_LP_FREQ_MHZ (0)
#define MAX_LP_FREQ_MHZ (2150)

#define MIN_BYPASS_FREQ_MHZ (2150)
#define MAX_BYPASS_FREQ_MHZ (2750)

#define MIN_HP_FREQ_MHZ (2750)
#define MID1_HP_FREQ_MHZ (3600)
#define MID2_HP_FREQ_MHZ (5100)
#define MAX_HP_FREQ_MHZ (7250)

#define MIN_LO_FREQ_HZ (84375000)
#define MAX_LO_FREQ_HZ (5400000000ULL)

static uint32_t max2837_freq_nominal_hz=2560000000;

uint64_t freq_cache = 100000000;
/*
 * Set freq/tuning between 0MHz to 7250 MHz (less than 16bits really used)
 * hz between 0 to 999999 Hz (not checked)
 * return false on error or true if success.
 */
bool set_freq(const uint64_t freq)
{
	bool success;
	uint32_t RFFC5071_freq_mhz;
	uint32_t MAX2837_freq_hz;
	uint64_t real_RFFC5071_freq_hz;

	const uint32_t freq_mhz = freq / 1000000;
	const uint32_t freq_hz = freq % 1000000;

	success = true;

	const max2837_mode_t prior_max2837_mode = max2837_mode(&max2837);
	max2837_set_mode(&max2837, MAX2837_MODE_STANDBY);
	if(freq_mhz < MAX_LP_FREQ_MHZ)
	{
		rf_path_set_filter(&rf_path, RF_PATH_FILTER_LOW_PASS);
		/* IF is graduated from 2650 MHz to 2343 MHz */
		max2837_freq_nominal_hz = 2650000000 - (freq / 7);
		RFFC5071_freq_mhz = (max2837_freq_nominal_hz / FREQ_ONE_MHZ) + freq_mhz;
		/* Set Freq and read real freq */
		real_RFFC5071_freq_hz = rffc5071_set_frequency(&rffc5072, RFFC5071_freq_mhz);
		max2837_set_frequency(&max2837, real_RFFC5071_freq_hz - freq);
		sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 1);
	}else if( (freq_mhz >= MIN_BYPASS_FREQ_MHZ) && (freq_mhz < MAX_BYPASS_FREQ_MHZ) )
	{
		rf_path_set_filter(&rf_path, RF_PATH_FILTER_BYPASS);
		MAX2837_freq_hz = (freq_mhz * FREQ_ONE_MHZ) + freq_hz;
		/* RFFC5071_freq_mhz <= not used in Bypass mode */
		max2837_set_frequency(&max2837, MAX2837_freq_hz);
		sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 0);
	}else if(  (freq_mhz >= MIN_HP_FREQ_MHZ) && (freq_mhz <= MAX_HP_FREQ_MHZ) )
	{
		if (freq_mhz < MID1_HP_FREQ_MHZ) {
			/* IF is graduated from 2150 MHz to 2750 MHz */
			max2837_freq_nominal_hz = 2150000000 + (((freq - 2750000000) * 60) / 85);
		} else if (freq_mhz < MID2_HP_FREQ_MHZ) {
			/* IF is graduated from 2350 MHz to 2650 MHz */
			max2837_freq_nominal_hz = 2350000000 + ((freq - 3600000000) / 5);
		} else {
			/* IF is graduated from 2500 MHz to 2738 MHz */
			max2837_freq_nominal_hz = 2500000000 + ((freq - 5100000000) / 9);
		}
		rf_path_set_filter(&rf_path, RF_PATH_FILTER_HIGH_PASS);
		RFFC5071_freq_mhz = freq_mhz - (max2837_freq_nominal_hz / FREQ_ONE_MHZ);
		/* Set Freq and read real freq */
		real_RFFC5071_freq_hz = rffc5071_set_frequency(&rffc5072, RFFC5071_freq_mhz);
		max2837_set_frequency(&max2837, freq - real_RFFC5071_freq_hz);
		sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 0);
	}else
	{
		/* Error freq_mhz too high */
		success = false;
	}
	max2837_set_mode(&max2837, prior_max2837_mode);
	if( success ) {
		freq_cache = freq;
	}
	return success;
}

#endif
