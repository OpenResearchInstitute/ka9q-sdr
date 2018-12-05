// $Id: am.c,v 1.39 2018/12/05 07:08:01 karn Exp $
// AM envelope demodulator thread for 'radio'
// Copyright Oct 9 2017, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <complex.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>

#include "misc.h"
#include "dsp.h"
#include "filter.h"
#include "radio.h"

void *demod_am(void *arg){
  pthread_setname("am");
  assert(arg != NULL);
  struct demod * const demod = arg;

  // Set derived (and other) constants
  float const samptime = demod->filter.decimate / (float)demod->input.samprate;  // Time between (decimated) samples

  // AGC
  // I originally just kept the carrier at constant amplitude
  // but this fails when selective fading takes out the carrier, resulting in loud, distorted audio
  int hangcount = 0;
  float const recovery_factor = dB2voltage(demod->agc.recovery_rate * samptime); // AGC ramp-up rate/sample
  //  float const attack_factor = dB2voltage(demod->agc.attack_rate * samptime);      // AGC ramp-down rate/sample
  int const hangmax = demod->agc.hangtime / samptime; // samples before AGC increase
  demod->agc.gain = dB2voltage(80.); // Empirical

  // DC removal from envelope-detected AM and coherent AM
  float DC_filter = 0;
  float const DC_filter_coeff = .0001;

  demod->output.channels = 1; // Mono

  // Detection filter
  struct filter_out * const filter = create_filter_output(demod->filter.in,NULL,demod->filter.decimate,COMPLEX);
  demod->filter.out = filter;
  set_filter(filter,samptime*demod->filter.low,samptime*demod->filter.high,demod->filter.kaiser_beta);

  while(!demod->terminate){
    // New samples
    execute_filter_output(filter);    
    if(!isnan(demod->sig.n0))
      demod->sig.n0 += .001 * (compute_n0(demod) - demod->sig.n0); // Update noise estimate
    else
      demod->sig.n0 = compute_n0(demod); // Happens at startup

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
      
      if(isnan(demod->agc.gain)){
	demod->agc.gain = demod->agc.headroom / DC_filter;
      } else if(demod->agc.gain * DC_filter > demod->agc.headroom){
	demod->agc.gain = demod->agc.headroom / DC_filter;
	hangcount = hangmax;
      } else if(hangcount != 0){
	hangcount--;
      } else {
	demod->agc.gain *= recovery_factor;
      }
      samples[n] = (samp - DC_filter) * demod->agc.gain;
    }
    send_mono_output(demod,samples,filter->olen);
    // Scale to each sample so baseband power will display correctly
    demod->sig.bb_power = (signal + noise) / (2*filter->olen);
  } // terminate
  delete_filter_output(filter);
  demod->filter.out = NULL;
  pthread_exit(NULL);
}
