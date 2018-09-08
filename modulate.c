// $Id: modulate.c,v 1.13 2018/08/29 01:34:15 karn Exp $
// Simple I/Q AM modulator - will eventually support other modes
// Copyright 2017, Phil Karn, KA9Q
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <complex.h>
#include <limits.h>
#include <fftw3.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "misc.h"
#include "dsp.h"
#include "filter.h"
#include "radio.h"

#define BLOCKSIZE 4096

float const scale = 1./SHRT_MAX;

int Samprate = 192000;

int Verbose = 0;

int main(int argc,char *argv[]){
#if 0 // Better done manually?
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");
#endif

  // Set defaults
  double frequency = 48000;
  double amplitude = -20;
  double sweep = 0;

  char *modtype = "am";
  int c;
  while((c = getopt(argc,argv,"f:a:s:r:vm:")) != EOF){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'r':
      Samprate = strtol(optarg,NULL,0);
      break;
    case 'f':
      frequency = strtod(optarg,NULL);
      break;
    case 'a':
      amplitude = strtod(optarg,NULL);
      break;
    case 's':
      sweep = strtod(optarg,NULL); // sweep rate, Hz/sec
      break;
    case 'm':
      modtype = optarg;
      break;
    }
  }
  float low;
  float high;
  float carrier;

  if(strcasecmp(modtype,"am") == 0){
    carrier = 1;
    high = +5000;
    low = -5000;
  } else if(strcasecmp(modtype,"usb") == 0){
    carrier = 0;
    high = +3000;
    low = 0;
  } else if(strcasecmp(modtype,"lsb") == 0){
    carrier = 0;
    high = 0;
    low = -3000;
  } else if(strcasecmp(modtype,"ame") == 0){
    // AM enhanced: upper sideband + carrier (as in CHU)
    carrier = 1;
    high = +3000;
    low = 0;
  } else {
    fprintf(stderr,"Unknown modulation %s\n",modtype);
    exit(1);
  }
  if(Verbose){
    fprintf(stderr,"%s modulation on %.1f Hz IF, swept %.1f Hz/s, amplitude %5.1f dBFS, filter blocksize %'d\n",
	    modtype,frequency,sweep,amplitude,BLOCKSIZE);
  }
  if(-frequency > low && -frequency < high){
    fprintf(stderr,"Warning: low carrier frequency may interfere with receiver DC suppression\n");
  }

  frequency *= 2*M_PI/Samprate;       // radians/sample
  amplitude = pow(10.,amplitude/20.); // Convert to amplitude ratio
  sweep *= 2*M_PI / ((double)Samprate*Samprate);  // radians/sample

  complex double phase_accel = csincos(sweep);
  complex double phase_step = csincos(frequency);
  complex double phase = 1;  
  int const L = BLOCKSIZE;
  int const M = BLOCKSIZE + 1;
  int const N = L + M - 1;

  complex float * const response = fftwf_alloc_complex(N);
  memset(response,0,N*sizeof(response[0]));
  {
    float gain = 4./N; // Compensate for FFT/IFFT scaling and 4x upsampling
    for(int i=0;i<N;i++){
      float f;
      f = Samprate * ((float)i/N);
      if(f > Samprate/2)
	f -= Samprate;
      if(f >= low && f <= high)
	response[i] = gain;
      else
	response[i] = 0;
    }
  }
  window_filter(L,M,response,3.0);
  struct filter_in * const filter_in = create_filter_input(L,M,REAL);
  struct filter_out * const filter_out = create_filter_output(filter_in,response,1,COMPLEX);
  

  while(1){
    int16_t samp[L/4];
    if(pipefill(0,samp,sizeof(samp)) <= 0)
      break;
    // Filter will upsample by 4x
    for(int j=0,i=0;i<L;){
      filter_in->input.r[i++] = samp[j++] * scale;
      filter_in->input.r[i++] = 0;
      filter_in->input.r[i++] = 0;
      filter_in->input.r[i++] = 0;      
    }
    // Form baseband signal (analytic for SSB, pure real for AM/DSB)
    execute_filter_input(filter_in);
    execute_filter_output(filter_out);
    
    // Add carrier, if present
    if(carrier != 0){
      for(int i=0;i<L;i++)
	filter_out->output.c[i] += carrier;
    }
    // Spin up to chosen carrier frequency
    for(int i=0;i<L;i++){
      filter_out->output.c[i] *= phase * amplitude;
      phase *= phase_step;
      phase_step *= phase_accel;
    }
    phase /= cabs(phase);
    phase_step /= cabs(phase_step);
    int16_t output[2*L];
    for(int i=0;i<L;i++){
      output[2*i] = crealf(filter_out->output.c[i]) * SHRT_MAX;
      output[2*i+1] = cimagf(filter_out->output.c[i]) * SHRT_MAX;
    }
    int wlen = write(1,output,sizeof(output));
    if(wlen != sizeof(output)){
      perror("write");
      break;
    }
  }
  delete_filter_input(filter_in);
  delete_filter_output(filter_out);
  exit(0);
}
