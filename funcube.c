// $Id: funcube.c,v 1.38 2018/07/11 06:56:37 karn Exp $
// Read from AMSAT UK Funcube Pro and Pro+ dongles
// Multicast raw 16-bit I/Q samples
// Accept control commands from UDP socket
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <stdio.h>
#include <alsa/asoundlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <locale.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "fcd.h"
#include "sdr.h"
#include "radio.h"
#include "misc.h"
#include "multicast.h"

struct sdrstate {
  // Stuff for sending commands
  void *phd;                 // Opaque pointer to type hid_device

  struct status status;     // Frequency and gain settings, grouped for transmission in RTP packet

  unsigned int intfreq;              // Nominal (uncorrected) tuner frequency

  // Analog gain settings
  int agc_holdoff;
  int agc_holdoff_count;

  float in_power;              // Running estimate of signal power - used only by display
  // Smoothed error estimates
  complex float DC;      // DC offset
  float sinphi;          // I/Q phase error
  float imbalance;       // Ratio of I power to Q power

  // ALSA parameters
  snd_pcm_t *sdr_handle;     // ALSA handle
  snd_pcm_hw_params_t *sdrparams; // ALSA parameters for A/D converter
  char sdr_name[50];         // ALSA name of associated audio device for A/D
  int overrun;               // A/D overrun count
};

int ADC_samprate = 192000;
float const SCALE16 = 1./32767.;
int Verbose;
int No_hold_open; // if set, close control between commands
int Dongle;       // Which of several funcube dongles to use
float const DC_alpha = 1.0e-6;  // high pass filter coefficient for DC offset estimates, per sample
float const Power_alpha= 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates

// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
int Blocksize = 240;
int Dongle = 0;
char *Locale;
double Calibration = 0;


void *fcd_command(void *arg);
int process_fc_command(char *,int);
double set_fc_LO(double);
double fcd_actual(unsigned int u32Freq);
int front_end_init(int dongle, int samprate,int L);
int get_adc(short *buffer,const int L);
void *display(void *arg);

int Rtp_sock; // Socket handle for sending real time stream *and* receiving commands
int Ctl_sock;
extern int Mcast_ttl;
struct sdrstate FCD;
pthread_t FCD_control_thread;
pthread_t Display_thread;
pthread_t AGC_thread;
void *agc(void *arg);

int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");

  char *dest = "239.1.2.1"; // Default for testing

  Locale = getenv("LANG");
  if(Locale == NULL || strlen(Locale) == 0)
    Locale = "en_US.UTF-8";

  int c;
  while((c = getopt(argc,argv,"c:d:vp:l:b:oR:T:")) != EOF){
    switch(c){
    case 'c':
      Calibration = strtod(optarg,NULL) * 1e-6; // Calibration offset in ppm
      break;
    case 'R':
      dest = optarg;
      break;
    case 'o':
      No_hold_open++; // Close USB control port between commands so fcdpp can be used
      break;
    case 'd':
      Dongle = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
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
    }
  }
  if(Verbose)
    fprintf(stderr,"funcube dongle %d: blocksize %d\n",Dongle,Blocksize);

  setlocale(LC_ALL,Locale);
  
  // Load/save calibration file
  {
    char *calfilename = NULL;
    
    if(asprintf(&calfilename,"%s/.radiostate/cal-funcube-%d",getenv("HOME"),Dongle) > 0){
      FILE *calfp = NULL;
      if(Calibration == 0){
	if((calfp = fopen(calfilename,"r")) != NULL){
	  if(fscanf(calfp,"%lg",&Calibration) < 1){
	    fprintf(stderr,"Can't read calibration from %s\n",calfilename);
	  }
	}
      } else {
	if((calfp = fopen(calfilename,"w")) != NULL){
	  fprintf(calfp,"%.6lg",Calibration);
	}
      }
      if(calfp)
	fclose(calfp);
      free(calfilename);
    }
  }
  

  // Set up RTP output socket
  Rtp_sock = setup_mcast(dest,1);
  if(Rtp_sock == -1){
    perror("Can't create multicast socket");
    exit(1);
  }
    
  // Set up control socket
  Ctl_sock = socket(AF_INET,SOCK_DGRAM,0);

  // bind control socket to next sequential port after our multicast source port
  struct sockaddr_in ctl_sockaddr;
  socklen_t siz = sizeof(ctl_sockaddr);
  if(getsockname(Rtp_sock,&ctl_sockaddr,&siz) == -1){
    perror("getsockname on ctl port");
    exit(1);
  }
  struct sockaddr_in locsock;
  locsock.sin_family = AF_INET;
  locsock.sin_port = htons(ntohs(ctl_sockaddr.sin_port)+1);
  locsock.sin_addr.s_addr = INADDR_ANY;
  bind(Ctl_sock,&locsock,sizeof(locsock));

  if(front_end_init(Dongle,ADC_samprate,Blocksize) == -1){
    fprintf(stderr,"front_end_init(%d,%d,%d) failed\n",Dongle,ADC_samprate,Blocksize);
    exit(1);
  }
  usleep(100000); // Let things settle
  pthread_create(&FCD_control_thread,NULL,fcd_command,&FCD);
  
  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        

  if(Verbose > 1)
    pthread_create(&Display_thread,NULL,display,NULL);

  time_t tt;
  time(&tt);
  long ssrc;
  ssrc = tt & 0xffffffff; // low 32 bits of clock time

  int timestamp = 0;
  int seq = 0;
  // Gain and phase corrections. These will be updated every block
  float gain_q = 1;
  float gain_i = 1;
  float secphi = 1;
  float tanphi = 0;
  struct timeval tp;
  gettimeofday(&tp,NULL);
  // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
  FCD.status.timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;

  pthread_create(&AGC_thread,NULL,agc,NULL);

  while(1){
    struct rtp_header rtp;
    memset(&rtp,0,sizeof(rtp));
    rtp.version = RTP_VERS;
    rtp.type = IQ_PT;         // ordinarily dynamically allocated
    rtp.ssrc = ssrc;
    rtp.seq = seq++;
    rtp.timestamp = timestamp;

    unsigned char buffer[16384]; // Pick a better value
    unsigned char *dp = buffer;

    dp = hton_rtp(dp,&rtp);
    dp = hton_status(dp,&FCD.status);
    signed short *sampbuf = (signed short *)dp;
    get_adc(sampbuf,Blocksize);
    dp += Blocksize * 2 * sizeof(*sampbuf);

    float i_energy=0, q_energy=0;
    complex float samp_sum = 0;
    float dotprod = 0;
    float rate_factor = Blocksize/(ADC_samprate * Power_alpha);
    
    for(int i=0;i<2*Blocksize;i += 2){
      complex float samp = CMPLXF(sampbuf[i],sampbuf[i+1]) * SCALE16;

      samp_sum += samp;

      // remove DC offset (which can be fractional)
      samp -= FCD.DC;

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
      
      sampbuf[i] = round(crealf(samp) * 32767);
      sampbuf[i+1] = round(cimagf(samp) * 32767);
    }



    if(send(Rtp_sock,buffer,dp - buffer,0) == -1){
      perror("send");
      // If we're sending to a unicast address without a listener, we'll get ECONNREFUSED
      // Sleep 1 sec to slow down the rate of these messages
      usleep(1000000);
    }
    timestamp += Blocksize;

#if 0
    // Get status timestamp from UNIX TOD clock -- but this might skew because of inexact sample rate
    gettimeofday(&tp,NULL);
    // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
    FCD.status.timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;
#else
    // Simply increment by number of samples
    // But what if we lose some? Then the clock will always be off
    FCD.status.timestamp += 1.e9 * Blocksize / ADC_samprate;
#endif


    // Update every block
    // estimates of DC offset, signal powers and phase error
    FCD.DC += DC_alpha * (samp_sum - Blocksize*FCD.DC);
    float block_energy = 0.5 * (i_energy + q_energy); // Normalize for complex pairs
    if(block_energy > 0){ // Avoid divisions by 0, etc
      FCD.in_power = block_energy/Blocksize; // Average A/D output power per channel  
      FCD.imbalance += rate_factor * ((i_energy / q_energy) - FCD.imbalance);
      float dpn = dotprod / block_energy;
      FCD.sinphi += rate_factor * (dpn - FCD.sinphi);
      gain_q = sqrtf(0.5 * (1 + FCD.imbalance));
      gain_i = sqrtf(0.5 * (1 + 1./FCD.imbalance));
      secphi = 1/sqrtf(1 - FCD.sinphi * FCD.sinphi); // sec(phi) = 1/cos(phi)
      tanphi = FCD.sinphi * secphi;                     // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
    }
  }
  // Can't really get here
  close(Rtp_sock);
  exit(0);
}


int front_end_init(int dongle, int samprate,int L){
  int r;
  unsigned int exact_rate;
  snd_pcm_uframes_t buffer_size;

  FCD.status.samprate = samprate;
  FCD.agc_holdoff = 0.5 * FCD.status.samprate / L; // Block AGC changes for 1.0 sec after each change - samprate might not be set yet?
  FCD.agc_holdoff_count = 0;

  if(Verbose)
    fprintf(stderr,"Funcube dongle: ");
  if((FCD.phd = fcdOpen(FCD.sdr_name,sizeof(FCD.sdr_name),dongle)) == NULL){
    perror("fcdOpen()");
    return -1;
  }
  if((r = fcdGetMode(FCD.phd)) == FCD_MODE_APP){
    char caps_str[100];
    fcdGetCapsStr(FCD.phd,caps_str);
    if(Verbose)
      fprintf(stderr,"ALSA name '%s', caps '%s'\n",FCD.sdr_name,caps_str);
  } else if(r == FCD_MODE_NONE){
    fprintf(stderr," No FCD detected!\n");
    r = -1;
    goto done;
  } else if (r == FCD_MODE_BL){
    fprintf(stderr," is in bootloader mode\n");
    r = -1;
    goto done;
  }
  // Set up sample stream through ALSA subsystem
  if(Verbose)
    fprintf(stderr,"adc_setup(%s): ",FCD.sdr_name);
  if((r = snd_pcm_open(&FCD.sdr_handle,FCD.sdr_name,SND_PCM_STREAM_CAPTURE,0)) < 0){
    fprintf(stderr,"error opening PCM device: %s\n",snd_strerror(r));
    perror("");
    r = -1;
    goto done;
  }

  if(FCD.sdrparams == NULL)
    snd_pcm_hw_params_malloc(&FCD.sdrparams);
  if(snd_pcm_hw_params_any(FCD.sdr_handle,FCD.sdrparams) < 0){
    perror("can't configure this PCM device");
    r = -1;
    goto done;
  }
  if(snd_pcm_hw_params_set_access(FCD.sdr_handle,FCD.sdrparams,SND_PCM_ACCESS_RW_INTERLEAVED) < 0){
    perror("error setting access");
    r = -1;
    goto done;
  }
  if(snd_pcm_hw_params_set_format(FCD.sdr_handle,FCD.sdrparams,SND_PCM_FORMAT_S16_LE)<0){
    perror("error setting format");
    r = -1;
    goto done;
  }
  exact_rate = FCD.status.samprate;
  if(snd_pcm_hw_params_set_rate_near(FCD.sdr_handle,FCD.sdrparams,&exact_rate,0) < 0){
    perror("error setting rate");
    r = -1;
    goto done;
  }
  FCD.status.samprate = exact_rate;
  if(snd_pcm_hw_params_set_channels(FCD.sdr_handle,FCD.sdrparams,2) < 0){
    perror("error setting channels");
    r = -1;
    goto done;
  }
  // We will generally read L-sample blocks at a time
  snd_pcm_uframes_t LL = L;
  if(snd_pcm_hw_params_set_period_size_near(FCD.sdr_handle,FCD.sdrparams,&LL,0) < 0){
    perror("error setting periods");
    r = -1;
    goto done;
  }
  buffer_size = 1<<18;
  if(snd_pcm_hw_params_set_buffer_size_near(FCD.sdr_handle,FCD.sdrparams,&buffer_size) < 0){
    perror("error setting buffersize");
    r = -1;
    goto done;
  }
  if(snd_pcm_hw_params(FCD.sdr_handle,FCD.sdrparams) < 0){
    perror("error setting HW params");
    r = -1;
    goto done;
  }
  if(Verbose)
    fprintf(stderr,"A/D buffer %'d complex samples (%'.1f ms @ %'lu S/s)\n",
	    (int)buffer_size,
	    1000.*(float)buffer_size/FCD.status.samprate,
	    (unsigned long)FCD.status.samprate);

  snd_pcm_prepare(FCD.sdr_handle); // Start A/D conversion
  r = 0;

 done:; // Also the abort target: close handle before returning
  if(No_hold_open && FCD.phd != NULL){
    fcdClose(FCD.phd);
    FCD.phd = NULL;
  }
  return r;
}

// Read buffer of samples from front end
// L is number of stero samples, so buffer must have 2*L elements
int get_adc(short *buffer,const int L){
  int r;

  // Read block of I/Q samples from A/D converter
  do {
    snd_pcm_state_t state;
    state = snd_pcm_state(FCD.sdr_handle);
    if(state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_PREPARED){
      FCD.overrun++;
      snd_pcm_prepare(FCD.sdr_handle);
    }
    if((r = snd_pcm_readi(FCD.sdr_handle,buffer,L)) < 0){
      fprintf(stderr,"funcube read error %s, reinit...\n",snd_strerror(r));
      front_end_init(Dongle,ADC_samprate,Blocksize);
      usleep(500000); // Just to keep from locking things up
    }
  } while(r != L);
  return 0;
}

// Process commands to change FCD state
// We listen on the same IP address and port we use as a multicasting source
void *fcd_command(void *arg){
  pthread_setname("funcube-cmd");

  while(1){
    fd_set fdset;
    socklen_t addrlen;
    int r;
    struct timeval timeout;
    struct status requested_status;
    
    // We're only reading one socket, but do it with a timeout so
    // we can periocally poll the Funcube Pro's status in case it
    // has been changed by another program, e.g., fcdpp
    FD_ZERO(&fdset);
    FD_SET(Ctl_sock,&fdset);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    r = select(Ctl_sock+1,&fdset,NULL,NULL,&timeout);
    if(r == -1){
      perror("select");
      sleep(50000); // don't loop tightly
      continue;
    } else if(r == 0){
      // Timeout: poll the FCD for its current state, in case it
      // has changed independently, e.g., from fcdctl command
      if(FCD.phd == NULL && (FCD.phd = fcdOpen(FCD.sdr_name,sizeof(FCD.sdr_name),Dongle)) == NULL){
	perror("funcube: can't re-open control port");
	sleep(50000);
	continue;
      }
      unsigned char val;
      fcdAppGetParam(FCD.phd,FCD_CMD_APP_GET_LNA_GAIN,&val,sizeof(val));
      FCD.status.lna_gain = val ? 1:0;

      fcdAppGetParam(FCD.phd,FCD_CMD_APP_GET_MIXER_GAIN,&val,sizeof(val));
      FCD.status.mixer_gain = val ? 19 : 0;

      fcdAppGetParam(FCD.phd,FCD_CMD_APP_GET_IF_GAIN1,&val,sizeof(val));
      FCD.status.if_gain = val;
      fcdAppGetParam(FCD.phd,FCD_CMD_APP_GET_FREQ_HZ,(unsigned char *)&FCD.intfreq,sizeof(FCD.intfreq));
      FCD.status.frequency = fcd_actual(FCD.intfreq) * (1 + Calibration);
      goto done;
    } 
    // A command arrived, read it
    // Should probably log these
    struct sockaddr_in6 command_address;
    addrlen = sizeof(command_address);
    if((r = recvfrom(Ctl_sock,&requested_status,sizeof(requested_status),0,&command_address,&addrlen)) <= 0){
      if(r < 0)
	perror("recv");
      sleep(50000); // don't loop tightly
      continue;
    }
      
    if(r < sizeof(requested_status))
      continue; // Too short; ignore
    
    if(FCD.phd == NULL && (FCD.phd = fcdOpen(FCD.sdr_name,sizeof(FCD.sdr_name),Dongle)) == NULL){
      perror("funcube: can't re-open control port, aborting");
      abort();
    }      

    // See what has changed, and set it in the hardware
    if(requested_status.lna_gain != 0xff && FCD.status.lna_gain != requested_status.lna_gain){
      FCD.status.lna_gain = requested_status.lna_gain;
#if 0
      fprintf(stderr,"lna %s\n",FCD.status.lna_gain ? "ON" : "OFF");
#endif
      unsigned char val = FCD.status.lna_gain ? 1 : 0;
      fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
    }
    if(requested_status.mixer_gain != 0xff && FCD.status.mixer_gain != requested_status.mixer_gain){
      FCD.status.mixer_gain = requested_status.mixer_gain;
#if 0
      fprintf(stderr,"mixer %s\n",FCD.status.mixer_gain ? "ON" : "OFF");
#endif
      unsigned char val = FCD.status.mixer_gain ? 1 : 0;
      fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    }
    if(requested_status.if_gain != 0xff && FCD.status.if_gain != requested_status.if_gain){
      FCD.status.if_gain = requested_status.if_gain;
#if 0
      fprintf(stderr,"IF gain %d db\n",FCD.status.if_gain);
#endif
      fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_IF_GAIN1,&FCD.status.if_gain,sizeof(FCD.status.if_gain));
    }
    if(requested_status.frequency > 0 && FCD.status.frequency != requested_status.frequency){
      
#if 0
      fprintf(stderr,"tuner frequency %lf\n",requested_status.frequency);
#endif
      FCD.intfreq = round(requested_status.frequency/ (1 + Calibration));
      fcdAppSetFreq(FCD.phd,FCD.intfreq);
      FCD.status.frequency = fcd_actual(FCD.intfreq) * (1 + Calibration);
    }
  done:;
    if(No_hold_open && FCD.phd != NULL){
      fcdClose(FCD.phd);
      FCD.phd = NULL;
    }
  }
}

// Status display thread
void *display(void *arg){
  pthread_setname("funcube-disp");

  fprintf(stderr,"               |---Gains dB---|      |----Levels dB --|   |---------Errors---------|           clips\n");
  fprintf(stderr,"Frequency      LNA  mixer bband          RF   A/D   Out     DC-I   DC-Q  phase  gain                        TCXO\n");
  fprintf(stderr,"Hz                                           dBFS  dBFS                    deg    dB                         ppm\n");   

  while(1){
    float powerdB = 10*log10f(FCD.in_power);

    fprintf(stderr,"%'-15.0lf%3d%7d%6d%'12.1f%'6.1f%'6.1f%9.4f%7.4f%7.2f%6.2f%'16d    %8.4lf\r",
	    FCD.status.frequency,
	    FCD.status.lna_gain,	    
	    FCD.status.mixer_gain,
	    FCD.status.if_gain,
	    powerdB - (FCD.status.lna_gain + FCD.status.mixer_gain + FCD.status.if_gain),
	    powerdB,
	    powerdB,
	    crealf(FCD.DC),
	    cimagf(FCD.DC),
	    (180/M_PI) * asin(FCD.sinphi),
	    10*log10(FCD.imbalance),
	    0,
	    Calibration * 1e6
	    );
    usleep(100000);
  }    

  return NULL;
}



// If we don't stop the A/D, it'll take several seconds to overflow and stop by itself,
// and during that time we can't restart
void closedown(int a){
  if(Verbose)
    fprintf(stderr,"funcube: caught signal %d: %s\n",a,strsignal(a));
  snd_pcm_drop(FCD.sdr_handle);
  exit(1);
}
// The funcube dongle uses the Mirics MSi001 tuner. It has a fractional N synthesizer that can't actually do integer frequency steps.
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
// average energy (I+Q) in each sample, current block, **including DC offset**
// At low levels, will disagree with demod's IF1 figure, which has the DC removed

void *agc(void *arg){
  while(1){
    usleep(100000);
  
    if(FCD.in_power > 1e-1){ // -20dBFS
      // reduce mixer gain first
      if(FCD.status.mixer_gain > 0){
	FCD.status.mixer_gain = 0;
	unsigned char val = FCD.status.mixer_gain ? 1 : 0;
	fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
      } else if(FCD.status.lna_gain > 0){
	FCD.status.lna_gain = 0;
	unsigned char val = FCD.status.lna_gain ? 1 : 0;
	fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
      }
    } else if(FCD.in_power < 1e-3){  // -60dBFS
      // Increase LNA gain first
      if(FCD.status.lna_gain == 0){
	FCD.status.lna_gain = 14;
	unsigned char val = FCD.status.lna_gain ? 1:0;
	fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
      } else if(FCD.status.mixer_gain == 0){
	FCD.status.mixer_gain = 19;
	unsigned char val = FCD.status.mixer_gain ? 1 : 0;
	fcdAppSetParam(FCD.phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
	}
    }
  }
  return NULL;
}
