// $Id: doppler.c,v 1.8 2018/07/06 06:06:12 karn Exp $
// Real-time doppler steering
// Copyright 2018, Phil Karn KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sys/time.h>

#include "misc.h"
#include "radio.h"


// Thread invoked by -d command line option to 'radio'
// Spawns specified command, extracts velocity and acceleration from its output
// Computes frequency and frequency rate and calls doppler set functions
void *doppler(void *arg){
  pthread_setname("doppler");
  if(arg == NULL)
    return NULL;
  struct demod * const demod = arg;

  if(demod->doppler_command == NULL)
    return NULL; // No doppler command
  FILE *input;
  char line[1024];
  double t,az,azrate,el,elrate,range,rangerate,rangeraterate;
  double rt;

  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,NULL);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL);  
  set_doppler(demod,0,0);

  while(1){
    pthread_testcancel();
    input = popen(demod->doppler_command,"r");
    if(input == NULL){
      usleep(1000000); // Don't spin tight
      continue;
    }
      
    while(fgets(line,sizeof(line),input) != NULL){
      int n = sscanf(line,"%lf %lf %lf %lf %lf %lf %lf %lf",
		     &t,&az,&azrate,&el,&elrate,&range,&rangerate,&rangeraterate);
      if(n != 8)
	continue;
      
      struct timeval tv;
      gettimeofday(&tv,NULL);
      rt = tv.tv_sec + tv.tv_usec * 1e-6;
      if(t < rt){
	//	fprintf(stderr,"skip %lf\n",t);
	continue;
      }
      if(t > rt){
	useconds_t s = 1000000 * (t - rt); // Wait until right time 
	usleep(s);
      }
      // Compute doppler and doppler rate
      double const c = 299792458;
      double const f = get_freq(demod);
      set_doppler(demod,f * -rangerate/c,f * -rangeraterate/c);
    }
    fclose(input); // and try again

    set_doppler(demod,0,0);
  }
  pthread_exit(NULL);
}
