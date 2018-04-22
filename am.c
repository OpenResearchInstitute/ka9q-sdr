// $Id: am.c,v 1.32 2018/04/22 18:18:02 karn Exp $
// AM envelope demodulator thread for 'radio'
// Copyright Oct 9 2017, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <complex.h>
#include <math.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>

#include "misc.h"
#include "filter.h"
#include "radio.h"
#include "audio.h"

void *demod_am(void *arg){
  pthread_setname("am");
  assert(arg != NULL);
  struct demod * const demod = arg;
  struct audio * const audio = &Audio; // Eventually pass this as an argument

  // Set derived (and other) constants
  float const samptime = demod->decimate / demod->samprate;  // Time between (decimated) samples

  // AGC
  // I originally just kept the carrier at constant amplitude
  // but this fails when selective fading takes out the carrier, resulting in loud, distorted audio
  int hangcount = 0;
  float const recovery_factor = dB2voltage(demod->recovery_rate * samptime); // AGC ramp-up rate/sample
  //  float const attack_factor = dB2voltage(demod->attack_rate * samptime);      // AGC ramp-down rate/sample
  int const hangmax = demod->hangtime / samptime; // samples before AGC increase
  if(isnan(demod->gain))
    demod->gain = dB2voltage(20.);

  // DC removal from envelope-detected AM and coherent AM
  float DC_filter = 0;
  float const DC_filter_coeff = .0001;

  demod->flags |= MONO; // Implies mono

  demod->snr = -INFINITY; // Not used

  // Detection filter
  struct filter_out * const filter = create_filter_output(demod->filter_in,NULL,demod->decimate,COMPLEX);
  demod->filter_out = filter;
  set_filter(filter,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);

  while(!demod->terminate){
    // New samples
    execute_filter_output(filter);    
    if(!isnan(demod->n0))
      demod->n0 += .001 * (compute_n0(demod) - demod->n0); // Update noise estimate
    else
      demod->n0 = compute_n0(demod); // Happens at startup

    // AM envelope detector
    float signal = 0;
    float noise = 0;
    float samples[filter->olen];
    for(int n=0; n<filter->olen; n++){
      float const sampsq = cnrmf(filter->output.c[n]);
      signal += sampsq;
      float samp = sqrtf(sampsq);
      
      // Remove carrier DC from audio
      // DC_filter will always be positive since sqrtf() is positive
      DC_filter += DC_filter_coeff * (samp - DC_filter);
      
      if(isnan(demod->gain)){
	demod->gain = demod->headroom / DC_filter;
      } else if(demod->gain * DC_filter > demod->headroom){
	demod->gain = demod->headroom / DC_filter;
	hangcount = hangmax;
      } else if(hangcount != 0){
	hangcount--;
      } else {
	demod->gain *= recovery_factor;
      }
      samples[n] = (samp - DC_filter) * demod->gain;
    }
    send_mono_audio(audio,samples,filter->olen);
    demod->bb_power = (signal + noise) / filter->olen;
  } // terminate
  delete_filter_output(filter);
  demod->filter_out = NULL;
  pthread_exit(NULL);
}
