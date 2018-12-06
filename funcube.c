// $Id: funcube.c,v 1.60 2018/12/05 07:08:01 karn Exp $
// Read from AMSAT UK Funcube Pro and Pro+ dongles
// Multicast raw 16-bit I/Q samples
// Accept control commands from UDP socket
// rewritten to use portaudio July 2018
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <stdio.h>
#include <stdarg.h>
#include <portaudio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <locale.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>

#include "fcd.h"
#include "sdr.h"
#include "radio.h"
#include "misc.h"
#include "status.h"
#include "multicast.h"

struct sdrstate {
  // Stuff for sending commands
  void *phd;               // Opaque pointer to type hid_device

  struct status status;    // Frequency and gain settings, grouped for transmission in RTP packet

  unsigned int intfreq;    // Nominal (uncorrected) tuner frequency

  float in_power;          // Running estimate of signal power
  // Smoothed error estimates
  complex float DC;      // DC offset
  float sinphi;          // I/Q phase error
  float imbalance;       // Ratio of I power to Q power
  double calibration;    // TCXO Offset (0 = on frequency)

  // portaudio parameters
  PaStream *Pa_Stream;       // Portaudio handle
  char sdr_name[50];         // name of associated audio device for A/D
  int overrun;               // A/D overrun count
  int overflows;
};

// constants, some of which you might want to tweak
float const AGC_upper = -15;
float const AGC_lower = -50;
int const ADC_samprate = 192000;
float const SCALE16 = 1./SHRT_MAX;
float const DC_alpha = 1.0e-6;  // high pass filter coefficient for DC offset estimates, per sample
float const Power_alpha = 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates
char const *Rundir = "/run/funcube"; // Where 'status' and 'pid' are created

// Variables set by command line options
int No_hold_open; // if set, close control between commands
// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
int Blocksize = 240;
int Device = 0;
char *Locale;
int Daemonize;
int Mcast_ttl = 1; // Don't send fast IQ streams beyond the local network by default

// Global variables
struct rtp_state Rtp;
int Rtp_sock;     // Socket handle for sending real time stream
int Nctl_sock;    // Socket handle for incoming commands
int Status_sock;  // Socket handle for outgoing status messages
struct sockaddr_storage Output_dest_address; // Multicast output socket

struct sdrstate FCD;
pthread_t Display_thread;
pthread_t AGC_thread;
pthread_t Status_thread;
pthread_t Ncmd_thread;
FILE *Status;
char *Status_filename;
char *Pid_filename;
char *Dest;
uint64_t Commands;


void errmsg(const char *fmt,...);
int process_fc_command(char *,int);
double set_fc_LO(double);
double fcd_actual(unsigned int u32Freq);
int front_end_init(struct sdrstate *,int,int,int);
int get_adc(short *buffer,const int L);
void *display(void *arg);
void *doagc(void *arg);
void *ncmd(void *arg);


int main(int argc,char *argv[]){
  struct sdrstate * const sdr = &FCD;

  Locale = getenv("LANG");
  if(Locale == NULL || strlen(Locale) == 0)
    Locale = "en_US.UTF-8";

  int c;
  int List_audio = 0;

  while((c = getopt(argc,argv,"dc:vl:b:oR:T:LI:S:")) != -1){
    switch(c){
    case 'd':
      Daemonize++;
      Status = NULL;
      break;
    case 'L':
      List_audio++;
      break;
    case 'c':
      sdr->calibration = strtod(optarg,NULL) * 1e-6; // Calibration offset in ppm
      break;
    case 'R':
      Dest = optarg;
      break;
    case 'o':
      No_hold_open++; // Close USB control port between commands so fcdpp can be used
      break;
    case 'I':
      Device = strtol(optarg,NULL,0);
      break;
    case 'v':
      if(!Daemonize)
	Status = stderr; // Could be overridden by status file argument below
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
      Rtp.ssrc = strtol(optarg,NULL,0);
      break;
    default:
    case '?':
      fprintf(stderr,"Unknown argument %c\n",c);
      exit(1);
      break;
    }
  }
  setlocale(LC_ALL,Locale);

  if(List_audio){
    // On stdout, not stderr, so we can toss ALSA's noisy error messages
    Pa_Initialize();
    int numDevices = Pa_GetDeviceCount();
    printf("%d Audio devices:\n",numDevices);
    for(int inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      printf("%d: %s\n",inDevNum,deviceInfo->name);
    }
    Pa_Terminate();
    exit(0);
  }
  if(Dest == NULL){
    errmsg("Must specify -R output_address\n");
    exit(1);
  }

  if(Daemonize){

    openlog("funcube",LOG_PID,LOG_DAEMON);
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
    }
    if(Status)
      setlinebuf(Status);
  }

  // Catch signals so portaudio can be shut down
  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGBUS,closedown);
  signal(SIGSEGV,closedown);

  // Load/save calibration file
  {
    char *calfilename = NULL;
    
    if(asprintf(&calfilename,"/var/local/lib/radiostate/cal-funcube-%d",Device) > 0){
      FILE *calfp = NULL;
      if(sdr->calibration == 0){
	if((calfp = fopen(calfilename,"r")) != NULL){
	  if(fscanf(calfp,"%lg",&sdr->calibration) < 1){
	    errmsg("Can't read calibration from %s\n",calfilename);
	  }
	}
      } else {
	if((calfp = fopen(calfilename,"w")) != NULL){
	  fprintf(calfp,"%.6lg\n",sdr->calibration);
	}
      }
      if(calfp)
	fclose(calfp);
      free(calfilename);
    }
  }
  // Set up RTP output socket
  sleep(2);
  Rtp_sock = setup_mcast(Dest,(struct sockaddr *)&Output_dest_address,1,Mcast_ttl,0);
  if(Rtp_sock == -1){
    errmsg("Can't create multicast socket: %s\n",strerror(errno));
    exit(1);
  }
    
  Pa_Initialize();
  if(front_end_init(sdr,Device,ADC_samprate,Blocksize) < 0){
    errmsg("front_end_init(%p,%d,%d,%d) failed\n",
	   sdr,Device,ADC_samprate,Blocksize);
    exit(1);
  }

  pthread_create(&Status_thread,NULL,status,sdr);
  pthread_create(&Ncmd_thread,NULL,ncmd,sdr);

  if(Status)
    pthread_create(&Display_thread,NULL,display,sdr);

  if(Rtp.ssrc == 0){
    time_t tt;
    time(&tt);
    Rtp.ssrc = tt & 0xffffffff; // low 32 bits of clock time
  }
  errmsg("uid %d; device %d; dest %s; blocksize %d; RTP SSRC %lx; status file %s\n",getuid(),Device,Dest,Blocksize,Rtp.ssrc,Status_filename);
  // Gain and phase corrections. These will be updated every block
  float gain_q = 1;
  float gain_i = 1;
  float secphi = 1;
  float tanphi = 0;
  struct timeval tp;
  gettimeofday(&tp,NULL);
  // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
  sdr->status.timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;

  float rate_factor = Blocksize/(ADC_samprate * Power_alpha);

  while(1){
    struct rtp_header rtp;
    memset(&rtp,0,sizeof(rtp));
    rtp.version = RTP_VERS;
    rtp.type = IQ_PT;         // ordinarily dynamically allocated
    rtp.ssrc = Rtp.ssrc;
    rtp.seq = Rtp.seq++;
    rtp.timestamp = Rtp.timestamp;

    unsigned char buffer[16384]; // Pick a better value
    unsigned char *dp = buffer;

    dp = hton_rtp(dp,&rtp);
    dp = hton_status(dp,&sdr->status); // This will soon be removed
    signed short *sampbuf = (signed short *)dp;

    // Read block of I/Q samples from A/D converter
    int r = Pa_ReadStream(sdr->Pa_Stream,sampbuf,Blocksize);
    if(r == paInputOverflowed)
      sdr->overflows++;

    dp += Blocksize * 2 * sizeof(*sampbuf);

    float i_energy=0, q_energy=0;
    complex float samp_sum = 0;
    float dotprod = 0;
    
    for(int i=0; i<2*Blocksize; i += 2){
      complex float samp = CMPLXF(sampbuf[i],sampbuf[i+1]) * SCALE16;
      //complex float samp = CMPLXF(sampbuf[i],sampbuf[i+1]);

      samp_sum += samp; // Accumulate average DC values
      samp -= sdr->DC;   // remove smoothed DC offset (which can be fractional)

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
      
      sampbuf[i] = round(crealf(samp) * SHRT_MAX);
      sampbuf[i+1] = round(cimagf(samp) * SHRT_MAX);
      //sampbuf[i] = round(crealf(samp));
      //sampbuf[i+1] = round(cimagf(samp));
    }

    if(send(Rtp_sock,buffer,dp - buffer,0) == -1){
      errmsg("send: %s\n",strerror(errno));
      // If we're sending to a unicast address without a listener, we'll get ECONNREFUSED
      // Should sleep to slow down the rate of these messages
    } else {
      Rtp.packets++;
      Rtp.bytes += Blocksize;
    }
    Rtp.timestamp += Blocksize;

#if 1
    // Get status timestamp from UNIX TOD clock -- but this might skew because of inexact sample rate
    gettimeofday(&tp,NULL);
    // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
    sdr->status.timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;
#else
    // Simply increment by number of samples
    // But what if we lose some? Then the clock will always be off
    sdr->status.timestamp += 1.e9 * Blocksize / ADC_samprate;
#endif

    // Update every block
    // estimates of DC offset, signal powers and phase error
    sdr->DC += DC_alpha * (samp_sum - Blocksize*sdr->DC);
    float block_energy = 0.5 * (i_energy + q_energy); // Normalize for complex pairs
    if(block_energy > 0){ // Avoid divisions by 0, etc
      sdr->in_power = block_energy/Blocksize; // Average A/D output power per channel  
      sdr->imbalance += rate_factor * ((i_energy / q_energy) - sdr->imbalance);
      float dpn = dotprod / block_energy;
      sdr->sinphi += rate_factor * (dpn - sdr->sinphi);
      gain_q = sqrtf(0.5 * (1 + sdr->imbalance));
      gain_i = sqrtf(0.5 * (1 + 1./sdr->imbalance));
      secphi = 1/sqrtf(1 - sdr->sinphi * sdr->sinphi); // sec(phi) = 1/cos(phi)
      // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
      tanphi = sdr->sinphi * secphi;
    }
  }
  // Can't really get here
  close(Rtp_sock);
  exit(0);
}


int front_end_init(struct sdrstate *sdr,int device, int samprate,int L){
  int r = 0;

  sdr->status.samprate = samprate;

  if((sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),device)) == NULL){
    errmsg("fcdOpen(%s): %s\n",sdr->sdr_name,strerror(errno));
    return -1;
  }
  if((r = fcdGetMode(sdr->phd)) == FCD_MODE_APP){
    char caps_str[100];
    fcdGetCapsStr(sdr->phd,caps_str);
    errmsg("audio device name '%s', caps '%s'\n",sdr->sdr_name,caps_str);
  } else if(r == FCD_MODE_NONE){
    errmsg(" No FCD detected!\n");
    r = -1;
    goto done;
  } else if (r == FCD_MODE_BL){
    errmsg(" is in bootloader mode\n");
    r = -1;
    goto done;
  }
  // Set up sample stream through portaudio subsystem
  // Search audio devices
  int numDevices = Pa_GetDeviceCount();
  int inDevNum = paNoDevice;
  for(int i = 0; i < numDevices; i++){
    const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(i);
    if(strstr(deviceInfo->name,sdr->sdr_name) != NULL){
      inDevNum = i;
      errmsg("portaudio name: %s\n",deviceInfo->name);
      break;
    }
  }
  if(inDevNum == paNoDevice){
    errmsg("Can't find portaudio name\n");
    r = -1;
    goto done;
  }
  PaStreamParameters inputParameters;
  memset(&inputParameters,0,sizeof(inputParameters));
  inputParameters.channelCount = 2;
  inputParameters.device = inDevNum;
  inputParameters.sampleFormat = paInt16;
  inputParameters.suggestedLatency = 0.020;
  r = Pa_OpenStream(&sdr->Pa_Stream,&inputParameters,NULL,ADC_samprate,
		    paFramesPerBufferUnspecified, 0, NULL, NULL);

  if(r < 0){
    errmsg("error opening PCM device: %s\n",strerror(errno));
    r = -1;
    goto done;
  }

  Pa_StartStream(sdr->Pa_Stream);

 done:; // Also the abort target: close handle before returning
  if(No_hold_open && sdr->phd != NULL){
    fcdClose(sdr->phd);
    sdr->phd = NULL;
  }
  return r;
}

// Status display thread
void *display(void *arg){
  pthread_setname("funcube-disp");
  off_t stat_point;
  char eol;
  long messages = 0;
  struct sdrstate *sdr = (struct sdrstate *)arg;

  fprintf(Status,"funcube daemon pid %d device %d\n",getpid(),Device);
  fprintf(Status,"               |---Gains dB---|      |----Levels dB --|   |---------Errors---------|           Overflows                messages\n");
  fprintf(Status,"Frequency      LNA  mixer bband          RF   A/D   Out     DC-I   DC-Q  phase  gain                        TCXO\n");
  fprintf(Status,"Hz                                           dBFS  dBFS                    deg    dB                         ppm\n");   

  stat_point = ftello(Status); // Current offset if file, -1 if terminal

  // End lines with return when writing to terminal, newlines when writing to status file
  eol = stat_point == -1 ? '\r' : '\n';

  while(1){
    //    float powerdB = 10*log10f(sdr->in_power) - 90.308734;
    float powerdB = 10*log10f(sdr->in_power);

    if(stat_point != -1)
      fseeko(Status,stat_point,SEEK_SET);
    fprintf(Status,"%'-15.0lf%3d%7d%6d%'12.1f%'6.1f%'6.1f%9.4f%7.4f%7.2f%6.2f%'16d    %8.4lf%'10ld%c",
	    sdr->status.frequency,
	    sdr->status.lna_gain,	    
	    sdr->status.mixer_gain,
	    sdr->status.if_gain,
	    powerdB - (sdr->status.lna_gain + sdr->status.mixer_gain + sdr->status.if_gain),
	    powerdB,
	    powerdB,
	    crealf(sdr->DC),
	    cimagf(sdr->DC),
	    (180/M_PI) * asin(sdr->sinphi),
	    10*log10(sdr->imbalance),
	    sdr->overflows,
	    sdr->calibration * 1e6,
	    messages,
	    eol
	    );
    messages++;
    fflush(Status);
    usleep(100000);
  }    

  return NULL;
}

// If we don't stop the A/D, it'll take several seconds to overflow and stop by itself,
// and during that time we can't restart
void closedown(int a){
  errmsg("funcube: caught signal %d: %s\n",a,strsignal(a));
  unlink(Pid_filename);
  Pa_Terminate();
  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    exit(0);

  exit(1);
}
// The funcube device uses the Mirics MSi001 tuner. It has a fractional N synthesizer that can't actually do integer frequency steps.
// This formula is hacked down from code from Howard Long; it's what he uses in the firmware so I can figure out
// the *actual* frequency. Of course, we still have to correct it for the TCXO offset.

// This needs to be generalized since other tuners will be completely different!
double fcd_actual(unsigned int u32Freq){
  typedef unsigned int UINT32;
  typedef unsigned long long UINT64;

  const UINT32 u32Thresh = 3250U;
  const UINT32 u32FRef = 26000000U;
  double f64FAct;
  
  struct
  {
    UINT32 u32Freq;
    UINT32 u32FreqOff;
    UINT32 u32LODiv;
  } *pts,ats[]=
      {
	{4000000U,130000000U,16U},
	{8000000U,130000000U,16U},
	{16000000U,130000000U,16U},
	{32000000U,130000000U,16U},
	{75000000U,130000000U,16U},
	{125000000U,0U,32U},
	{142000000U,0U,16U},
	{148000000U,0U,16U},
	{300000000U,0U,16U},
	{430000000U,0U,4U},
	{440000000U,0U,4U},
	{875000000U,0U,4U},
	{UINT32_MAX,0U,2U},
	{0U,0U,0U}
      };
  for(pts = ats; u32Freq >= pts->u32Freq; pts++)
    ;

  if (pts->u32Freq == 0)
    pts--;
      
  // Frequency of synthesizer before divider - can possibly exceed 32 bits, so it's stored in 64
  UINT64 u64FSynth = ((UINT64)u32Freq + pts->u32FreqOff) * pts->u32LODiv;

  // Integer part of divisor ("INT")
  UINT32 u32Int = u64FSynth / (u32FRef*4);

  // Subtract integer part to get fractional and AFC parts of divisor ("FRAC" and "AFC")
  UINT32 u32Frac4096 =  (u64FSynth<<12) * u32Thresh/(u32FRef*4) - (u32Int<<12) * u32Thresh;

  // FRAC is higher 12 bits
  UINT32 u32Frac = u32Frac4096>>12;

  // AFC is lower 12 bits
  UINT32 u32AFC = u32Frac4096 - (u32Frac<<12);
      
  // Actual tuner frequency, in floating point, given specified parameters
  f64FAct = (4.0 * u32FRef / (double)pts->u32LODiv) * (u32Int + ((u32Frac * 4096.0 + u32AFC) / (u32Thresh * 4096.))) - pts->u32FreqOff;
  
  // double f64step = ( (4.0 * u32FRef) / (pts->u32LODiv * (double)u32Thresh) ) / 4096.0;
  //      printf("f64step = %'lf, u32LODiv = %'u, u32Frac = %'d, u32AFC = %'d, u32Int = %'d, u32Thresh = %'d, u32FreqOff = %'d, f64FAct = %'lf err = %'lf\n",
  //	     f64step, pts->u32LODiv, u32Frac, u32AFC, u32Int, u32Thresh, pts->u32FreqOff,f64FAct,f64FAct - u32Freq);
  return f64FAct;
}

// Crude analog AGC just to keep signal roughly within A/D range
// Executed only if -o option isn't specified; this allows manual control with, e.g., the fcdpp command
void *doagc(void *arg){
  struct sdrstate * const sdr = (struct sdrstate *)arg;

  float powerdB = 10*log10f(sdr->in_power);
  
  if(powerdB > AGC_upper){
    if(sdr->status.if_gain > 0){
      // Decrease gain in 10 dB steps, down to 0
      unsigned char val = sdr->status.if_gain = max(0,sdr->status.if_gain - 10);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
    } else if(sdr->status.mixer_gain){
      unsigned char val = sdr->status.mixer_gain = 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    } else if(sdr->status.lna_gain){
      unsigned char val = sdr->status.lna_gain = 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
    }
  } else if(powerdB < AGC_lower){
    if(sdr->status.lna_gain == 0){
      sdr->status.lna_gain = 24;
      unsigned char val = 1;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
    } else if(sdr->status.mixer_gain == 0){
      sdr->status.mixer_gain = 19;
      unsigned char val = 1;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    } else if(sdr->status.if_gain < 20){ // Limit to 20 dB - seems enough to keep A/D going even on noise
      unsigned char val = sdr->status.if_gain = min(20,sdr->status.if_gain + 10);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
    }
  }
  return NULL;
}
void errmsg(const char *fmt,...){
  va_list ap;

  va_start(ap,fmt);

  if(Daemonize)
    vsyslog(LOG_INFO,fmt,ap);
  else {
    vfprintf(stderr,fmt,ap);
    fflush(stderr);
  }
  va_end(ap);
}


#if 0
// AGC code fragment that uses Mirics gain tables
// This has problems, probably because of extra gain stages ahead of the Mirics tuner
int oldagc(struct sdrstate *sdr){

    int change = 0;

    // Hysteresis to keep AGC from changing too often
    if(powerdB > AGC_upper){ // AGC upper limit
      change = round(AGC_upper - powerdB);
    } else if(powerdB < AGC_lower){ // AGC lower limit
      change = round(AGC_lower - powerdB);
    } else
      goto done;
    
    int old_lna_gain = sdr->status.lna_gain;
    int old_mixer_gain = sdr->status.mixer_gain;
    int old_if_gain = sdr->status.if_gain;

    // Use Mirics gain map
    int newgain = old_if_gain + old_mixer_gain + old_lna_gain + change;

    // For all frequencies, start by turning up the IF gain until the mixer goes on at 19 dB
    if(newgain < 20)
      sdr->status.mixer_gain = 0;
    else
      sdr->status.mixer_gain = 19;

    if(sdr->status.frequency < 60e6){ // 60 MHz
      if(newgain <= 67)
	sdr->status.lna_gain = 0;
      else
	sdr->status.lna_gain = 24;	
    } else if(sdr->status.frequency < 120e6){ // 120 MHz
      if(newgain <= 73)
	sdr->status.lna_gain = 0;
      else
	sdr->status.lna_gain = 24;	
    } else if(sdr->status.frequency < 250e6){ // 250 MHz
      if(newgain <= 67)
	sdr->status.lna_gain = 0;
      else
	sdr->status.lna_gain = 24;	
    } else if(sdr->status.frequency < 1e9){ // 1 GHz
      if(newgain <= 73)
	sdr->status.lna_gain = 0;
      else
	sdr->status.lna_gain = 7;	
    } else {
      //if(sdr->status.frequency < 2e9){ // 2 GHz
      if(newgain <= 75)
	sdr->status.lna_gain = 0;
      else
	sdr->status.lna_gain = 7;	
    }
    sdr->status.if_gain = newgain - sdr->status.lna_gain - sdr->status.mixer_gain;
#if 0
    if(sdr->status.if_gain >= 60)
      sdr->status.if_gain = 59; // Limited to +59 dB 
#else
    if(sdr->status.if_gain >= 10)
      sdr->status.if_gain = 9; // Limited to +9 dB - hack
#endif

    // Apply any changes
    if(old_lna_gain != sdr->status.lna_gain){
      unsigned char val = sdr->status.lna_gain ? 1 : 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
    }
    if(old_mixer_gain != sdr->status.mixer_gain){
      unsigned char val = sdr->status.mixer_gain ? 1 : 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    }
    if(old_if_gain != sdr->status.if_gain){
      unsigned char val = sdr->status.if_gain;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
    }
  done:;
}
#endif


void *ncmd(void *arg){
  pthread_setname("new-cmd");
  assert(arg != NULL);
  struct sdrstate * const sdr = arg;
  
  // Set up new control socket on port 5006
  Nctl_sock = setup_mcast(Dest,NULL,0,Mcast_ttl,2); // For input

  while(1){
    if(sdr->phd == NULL && (sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),Device)) == NULL){
      errmsg("can't re-open control port: %s\n",strerror(errno));
      sleep(5);
      continue;
    }

    // Read back FCD state every iteration, whether or not we processed a command, just in case it was set by another program
    unsigned char val;
    fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_LNA_GAIN,&val,sizeof(val));
    if(val){
      if(sdr->intfreq >= 420000000)
	sdr->status.lna_gain = 7;
      else
	sdr->status.lna_gain = 24;
    } else
      sdr->status.lna_gain = 0;
    
    fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_MIXER_GAIN,&val,sizeof(val));
    sdr->status.mixer_gain = val ? 19 : 0;
    
    fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_IF_GAIN1,&val,sizeof(val));
    sdr->status.if_gain = val;

    fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_FREQ_HZ,(unsigned char *)&sdr->intfreq,sizeof(sdr->intfreq));
    sdr->status.frequency = fcd_actual(sdr->intfreq) * (1 + sdr->calibration);

    // Need to do agc in separate thread, with interlocking on funcube control port
    if(!No_hold_open)
      doagc(sdr);

    if(No_hold_open && sdr->phd != NULL){
      fcdClose(sdr->phd);
      sdr->phd = NULL;
    }

    if(Nctl_sock <= 0)
      return NULL; // Nothing to do

    unsigned char buffer[8192];
    memset(buffer,0,sizeof(buffer));
    int length = recv(Nctl_sock,buffer,sizeof(buffer),0);
    if(length <= 0){
      sleep(1);
      continue;
    }
    // Parse entries
    unsigned char *cp = buffer;

    int cr = *cp++; // Command/response
    if(cr == 0)
      continue; // Ignore our own status messages

    Commands++;
    while(cp - buffer < length){
      enum status_type type = *cp++; // increment cp to length field
    
      if(type == EOL)
	break; // End of list

      unsigned int len = *cp++;
      if(cp - buffer + len >= length)
	break; // Invalid length

      unsigned char val;
      switch(type){
      case EOL: // Shouldn't get here
	break;
      case CALIBRATE:
	sdr->calibration = decode_double(cp,len);
	break;
      case RADIO_FREQUENCY:
	sdr->status.frequency = decode_double(cp,len);
	sdr->intfreq = round(sdr->status.frequency/ (1 + sdr->calibration));
	// LNA gain is frequency-dependent
	if(sdr->status.lna_gain){
	  if(sdr->intfreq >= 420e6)
	    sdr->status.lna_gain = 7;
	  else
	    sdr->status.lna_gain = 24;
	}
	fcdAppSetFreq(sdr->phd,sdr->intfreq);
	sdr->status.frequency = fcd_actual(sdr->intfreq) * (1 + sdr->calibration);
	break;
      case LNA_GAIN:
	sdr->status.lna_gain = decode_int(cp,len);
	val = sdr->status.lna_gain ? 1 : 0;
	fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
	break;
      case MIXER_GAIN:
	sdr->status.mixer_gain = decode_int(cp,len);
	val = sdr->status.mixer_gain ? 1 : 0;
	fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
	break;
      case IF_GAIN:
	sdr->status.if_gain = decode_int(cp,len);
	fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&sdr->status.if_gain,sizeof(sdr->status.if_gain));
	break;
      default: // Ignore all others
	break;
      }

    }
  }
}


struct state State[256];

// Thread to periodically transmit our state
void *status(void *arg){
  pthread_setname("funcube-status");
  assert(arg != NULL);
  struct sdrstate * const sdr = arg;

  memset(State,0,sizeof(State));
  
  // Set up status socket on port 5006
  Status_sock = setup_mcast(Dest,NULL,1,Mcast_ttl,2);

  for(int count=0;;count++){
    if(Status_sock <= 0)
      return NULL; // Nothing we can do, so quit

    // emit status packets indefinitely
    unsigned char packet[2048],*bp;
    memset(packet,0,sizeof(packet));
    bp = packet;

    *bp++ = 0; // command/response = response

    struct timeval tp;
    gettimeofday(&tp,NULL);
    // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
    long long timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;
    encode_int64(&bp,GPS_TIME,timestamp);
    encode_int64(&bp,COMMANDS,Commands);
    // Where we're sending output
    // Right now the metadata and data are both sent to Output_dest_address, but I may add
    // the option to make them different. Then Output_dest_address will refer to the data stream
    // ie., the users seeing this metadata stream will know where to look for the data stream
    {
      struct sockaddr_in *sin;
      struct sockaddr_in6 *sin6;
      *bp++ = OUTPUT_DEST_SOCKET;
      switch(Output_dest_address.ss_family){
      case AF_INET:
	sin = (struct sockaddr_in *)&Output_dest_address;
	*bp++ = 6;
	memcpy(bp,&sin->sin_addr.s_addr,4); // Already in network order
	bp += 4;
	memcpy(bp,&sin->sin_port,2);
	bp += 2;
	break;
      case AF_INET6:
	sin6 = (struct sockaddr_in6 *)&Output_dest_address;
	*bp++ = 10;
	memcpy(bp,&sin6->sin6_addr,8);
	bp += 8;
	memcpy(bp,&sin6->sin6_port,2);
	bp += 2;
	break;
      default:
	break;
      }
    }
    encode_int32(&bp,OUTPUT_SSRC,Rtp.ssrc);
    encode_byte(&bp,OUTPUT_TTL,Mcast_ttl);
    encode_int32(&bp,OUTPUT_SAMPRATE,ADC_samprate);
    encode_int64(&bp,OUTPUT_PACKETS,Rtp.packets);

    // Tuning
    encode_double(&bp,RADIO_FREQUENCY,sdr->status.frequency);
    encode_double(&bp,CALIBRATE,sdr->calibration);

    // Front end
    encode_byte(&bp,LNA_GAIN,sdr->status.lna_gain);
    encode_byte(&bp,MIXER_GAIN,sdr->status.mixer_gain);
    encode_byte(&bp,IF_GAIN,sdr->status.if_gain);
    encode_float(&bp,DC_I_OFFSET,crealf(sdr->DC));
    encode_float(&bp,DC_Q_OFFSET,cimagf(sdr->DC));
    encode_float(&bp,IQ_IMBALANCE,sdr->imbalance);
    encode_float(&bp,IQ_PHASE,sdr->sinphi);

    // Filtering
    encode_float(&bp,LOW_EDGE,-90.0e3);
    encode_float(&bp,HIGH_EDGE,+90.0e3);

    // Signals - these ALWAYS change
    encode_float(&bp,BASEBAND_POWER,sdr->in_power);

    // Demodulation mode
    enum demod_type demod_type = LINEAR_DEMOD;
    encode_byte(&bp,DEMOD_MODE,demod_type);
    encode_int32(&bp,OUTPUT_CHANNELS,2);

    
    encode_eol(&bp);

    int len = compact_packet(&State[0],packet,(count % 10) == 0);
    //int len = bp - packet;
    send(Status_sock,packet,len,0);
    usleep(100000);
  }
}
