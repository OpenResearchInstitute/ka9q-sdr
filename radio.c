// $Id: radio.c,v 1.95 2018/07/11 06:58:45 karn Exp karn $
// Core of 'radio' program - control LOs, set frequency/mode, etc
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#undef I
#include <netinet/in.h>

#include "misc.h"
#include "dsp.h"
#include "radio.h"
#include "filter.h"


// SDR alias keep-out region, i.e., stay between -(samprate/2 - IF_EXCLUDE) and (samprate/2 - IF_EXCLUDE)
const float IF_EXCLUDE = 0.95; // Assume decimation filters roll off above Fs/2 * IF_EXCLUDE

// Preferred A/D sample rate; ignored by funcube but may be used by others someday
const int ADC_samprate = 192000;

// thread for first half of demodulator
// Preprocessing of samples performed for all demodulators
// Remove DC biases, equalize I/Q power, correct phase imbalance
// Update power measurement
// Pass to input of pre-demodulation filter

float const DC_alpha = 1e-6;    // high pass filter coefficient for DC offset estimates, per sample
float const Power_alpha= 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates

float const SCALE16 = 1./SHRT_MAX; // Scale signed 16-bit int to float in range -1, +1
float const SCALE8 = 1./127;       // Scale signed 8-bit int to float in range -1, +1

int is_phasor_init(const complex double x);

void *proc_samples(void *arg){
  // gain and phase balance coefficients
  assert(arg);
  pthread_setname("procsamp");

  struct demod *demod = (struct demod *)arg;

  float samp_i_sum = 0, samp_q_sum = 0;        // sums of I and Q, for DC offset
  float samp_i_sq_sum = 0, samp_q_sq_sum = 0;  // sums of I^2 and Q^2, for power and gain balance
  float dotprod = 0;                           // sum of I*Q, for phase balance

  // Gain and phase corrections. These will be updated every block
  float gain_q = 1;
  float gain_i = 1;
  float secphi = 1;
  float tanphi = 0;
  int in_cnt = 0;
  struct packet *pkt = NULL;


  while(1){
    // Pull next I/Q data packet off queue
    pthread_mutex_lock(&demod->qmutex);
    while(demod->queue == NULL)
      pthread_cond_wait(&demod->qcond,&demod->qmutex);
    pkt = demod->queue;
    demod->queue = pkt->next;
    pthread_mutex_unlock(&demod->qmutex);

    int sampcount;

    switch(pkt->rtp.type){
    default: // Shut up lint
    case IQ_PT:
      sampcount = pkt->len / (2 * sizeof(signed short));
      break;
    case IQ_PT8:
      sampcount = pkt->len / (2 * sizeof(signed char));
      break;
    }
    int time_step = rtp_process(&demod->rtp_state,&pkt->rtp,sampcount);
    if(time_step < 0 || time_step > 192000){
      // Old samples, or too big a jump; drop. Shouldn't happen if sequence number isn't old
      free(pkt); pkt = NULL;
      continue;
    } else if(time_step > 0){
      // Samples were lost. Inject enough zeroes to keep the sample count and LO phase correct
      // Arbitrary 1 sec limit just to keep things from blowing up
      // Good enough for the occasional lost packet or two
      // May upset the I/Q DC offset and channel balance estimates, but hey you can't win 'em all
      // Note: we don't use marker bits since we don't suppress silence
      demod->samples += time_step;
      for(int i=0;i < time_step; i++){
	demod->filter_in->input.c[in_cnt++] = 0;
	// Keep the LOs running
	demod->second_LO_phasor *= demod->second_LO_phasor_step;
	if(is_phasor_init(demod->doppler_phasor)){ // Initialized?
	  demod->doppler_phasor *= demod->doppler_phasor_step;
	  demod->doppler_phasor_step *= demod->doppler_phasor_step_step;
	}
	if(in_cnt == demod->filter_in->ilen){
	  // Run filter but freeze everything else?
	  execute_filter_input(demod->filter_in);
	  in_cnt = 0;
	}
      }
    }
    // Process individual samples
    signed short *sp = (signed short *)pkt->data;
    signed char *cp = (signed char *)pkt->data;
    demod->samples += sampcount;

    while(sampcount--){

      float samp_i,samp_q;

      switch(pkt->rtp.type){
      default: // shuts up lint
      case IQ_PT:
	samp_i = *sp++ * SCALE16;
	samp_q = *sp++ * SCALE16;
	break;
      case IQ_PT8:
	samp_i = *cp++ * SCALE8;
	samp_q = *cp++ * SCALE8;
	break;
      }
      if(SDR_correct){
	// Remove and update DC offsets
	samp_i_sum += samp_i;
	samp_q_sum += samp_q;
	samp_i -= demod->DC_i;
	samp_q -= demod->DC_q;
      }
	
      // accumulate I and Q energies before gain correction
      samp_i_sq_sum += samp_i * samp_i;
      samp_q_sq_sum += samp_q * samp_q;
	
      if(SDR_correct){
	// Balance gains, keeping constant total energy
	samp_i *= gain_i;                  samp_q *= gain_q;
	
	// Accumulate phase error
	dotprod += samp_i * samp_q;
	
	// Correct phase
	samp_q = secphi * samp_q - tanphi * samp_i;
      }
      // Scale down according to analog gain from SDR front end
      complex float samp = CMPLXF(samp_i,samp_q) * demod->gain_factor;

#if 0
      // Experimental notch filter
      if(demod->nf)
	samp = notch(demod->nf,samp);
#endif

      // Apply 2nd LO
      samp *= demod->second_LO_phasor;
      demod->second_LO_phasor *= demod->second_LO_phasor_step;
      
      // Apply Doppler if active
      if(is_phasor_init(demod->doppler_phasor)){ // Initialized?
	samp *= demod->doppler_phasor;
	demod->doppler_phasor *= demod->doppler_phasor_step;
	demod->doppler_phasor_step *= demod->doppler_phasor_step_step;
      }
      // Accumulate in filter input buffer
      demod->filter_in->input.c[in_cnt++] = samp;
      if(in_cnt == demod->filter_in->ilen){
	// Filter buffer is full, execute it
	execute_filter_input(demod->filter_in);
	float block_energy = 0.5 * (samp_i_sq_sum + samp_q_sq_sum); // Scale for two components per complex sample
	demod->level = block_energy / in_cnt; // Raw A/D level, without analog gain adjustment
	// gain_factor is a voltage ratio, square to get power ratio
	demod->if_power = demod->gain_factor * demod->gain_factor * demod->level; // Signal level after gain adjustment

	if(SDR_correct){
	  // Update every fft block
	  // estimates of DC offset, signal powers and phase error
	  float smooth_factor = in_cnt/(demod->samprate * Power_alpha);
	  if(isnan(smooth_factor) || isinf(smooth_factor))
	    smooth_factor = in_cnt;

	  demod->DC_i += DC_alpha * (samp_i_sum - in_cnt * demod->DC_i);
	  demod->DC_q += DC_alpha * (samp_q_sum - in_cnt * demod->DC_q);
	  demod->imbalance += smooth_factor * ((samp_i_sq_sum / samp_q_sq_sum) - demod->imbalance);
	  float dpn = dotprod / block_energy;
	  demod->sinphi += smooth_factor * (dpn - demod->sinphi);
	  gain_q = sqrtf(0.5 * (1 + demod->imbalance));
	  gain_i = sqrtf(0.5 * (1 + 1./demod->imbalance));
	  secphi = 1/sqrtf(1 - demod->sinphi * demod->sinphi); // sec(phi) = 1/cos(phi)
	  tanphi = demod->sinphi * secphi;                     // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
	  
	  // Reset for next block
	  samp_i_sum = 0;
	  samp_q_sum = 0;
	  dotprod = 0;
	}
	in_cnt = 0;
	samp_i_sq_sum = 0;
	samp_q_sq_sum = 0;
	
	// Renormalize phasors
	demod->second_LO_phasor /= cabs(demod->second_LO_phasor); // renormalize every block
	if(is_phasor_init(demod->doppler_phasor)){
	  demod->doppler_phasor /= cabs(demod->doppler_phasor);
	  demod->doppler_phasor_step /= cabs(demod->doppler_phasor_step);
	}
      } // Every FFT block
    } // for each sample in I/Q packet
    free(pkt); pkt = NULL;
  } // end of main loop
}

// Get true first LO frequency, with TCXO offset applied
double const get_first_LO(const struct demod * const demod){
  if(demod == NULL)
    return NAN;
	 
  return demod->status.frequency;
}


// Return second (software) local oscillator frequency
double get_second_LO(struct demod * const demod){
  if(demod == NULL)
    return NAN;
  pthread_mutex_lock(&demod->second_LO_mutex);
  double f = demod->second_LO;
  pthread_mutex_unlock(&demod->second_LO_mutex);  
  return f;
}

// Return actual radio frequency
double get_freq(struct demod * const demod){
  if(demod == NULL)
    return NAN;
  return demod->freq;
}

// Set a Doppler offset and sweep rate
int set_doppler(struct demod * const demod,double freq,double rate){
  pthread_mutex_lock(&demod->doppler_mutex);
  demod->doppler = freq;
  demod->doppler_rate = rate;
  demod->doppler_phasor_step = csincos(-2*M_PI*freq / demod->samprate);
  demod->doppler_phasor_step_step = csincos(-2*M_PI*rate / (demod->samprate*demod->samprate));
  if(!is_phasor_init(demod->doppler_phasor)) // Initialized?
    demod->doppler_phasor = 1;
  pthread_mutex_unlock(&demod->doppler_mutex);
  return 0;
}
double get_doppler(struct demod * const demod){
  pthread_mutex_lock(&demod->doppler_mutex);
  double f = demod->doppler;
  pthread_mutex_unlock(&demod->doppler_mutex);  
  return f;
}
double get_doppler_rate(struct demod * const demod){
  pthread_mutex_lock(&demod->doppler_mutex);
  double f = demod->doppler_rate;
  pthread_mutex_unlock(&demod->doppler_mutex);  
  return f;
}

// Set receiver frequency with optional first IF selection
// new_lo2 == NAN is a "don't care"; we'll try to pick a new LO2 that avoids retuning LO1.
// If that isn't possible we'll pick a default, (usually +/- 48 kHz, samprate/4)
// that moves the tuner the least
double set_freq(struct demod * const demod,double const f,double new_lo2){
  if(demod == NULL)
    return NAN;

  demod->freq = f;

  // Wait for sample rate to be known
  pthread_mutex_lock(&demod->status_mutex);
  while(demod->status.samprate == 0)
    pthread_cond_wait(&demod->status_cond,&demod->status_mutex);
  pthread_mutex_unlock(&demod->status_mutex);

  // No alias checking on explicitly provided lo2
  if(isnan(new_lo2) || !LO2_in_range(demod,new_lo2,0)){
    // Determine new LO2
    new_lo2 = -(f - get_first_LO(demod));
    // If the required new LO2 is out of range, retune LO1
    if(!LO2_in_range(demod,new_lo2,1)){
      // Pick new LO2 to minimize change in LO1 in case another receiver is using it
      new_lo2 = demod->status.samprate/4.;
      double LO1 = get_first_LO(demod);

      if(fabs(f + new_lo2 - LO1) > fabs(f - new_lo2 - LO1))
	new_lo2 = -new_lo2;
    }
  }
  double new_lo1 = f + new_lo2;
  // returns actual frequency, which may be different from requested because
  // of calibration offset and quantization error in the fractional-N synthesizer
  double actual_lo1 = set_first_LO(demod,new_lo1);
  new_lo2 += (actual_lo1 - new_lo1); // fold the difference into LO2
    
  // If front end doesn't retune don't retune LO2 either (e.g., when receiving from a recording)
  if(LO2_in_range(demod,new_lo2,0))
    set_second_LO(demod,new_lo2);

  return f;
}

// Set first (front end tuner) oscillator
// Note: single precision floating point is not accurate enough at VHF and above
// demod->first_LO is NOT updated here!
// It is set by incoming status frames so this will take time
double set_first_LO(struct demod * const demod,double first_LO){
  if(demod == NULL)
    return NAN;

  double current_lo1 = get_first_LO(demod);

  // Just return actual frequency without changing anything
  if(first_LO == current_lo1 || first_LO <= 0 || demod->tuner_lock || demod->input_source_address.sa_family != AF_INET)
    return first_LO;

  demod->requested_status.frequency = first_LO;

  // These need a way to set
  // The Funcube dongle has a very wide dynamic range, so it is rarely necessary
  // to change its analog gain settings and the 'radio' program never does
  // Right now, gain is only changed inside the 'funcube' program in response to actual A/D overload, which is rare
  demod->requested_status.samprate = ADC_samprate; // Preferred samprate; ignored by funcube
  demod->requested_status.lna_gain = 0xff;    // 0xff means "don't change"
  demod->requested_status.mixer_gain = 0xff;
  demod->requested_status.if_gain = 0xff;
  
  // If we know the sender, send it a tuning request
  struct sockaddr_in sdraddr;
  memcpy(&sdraddr,&demod->input_source_address,sizeof(sdraddr));
  sdraddr.sin_port = htons(ntohs(sdraddr.sin_port)+1);
  if(sendto(demod->ctl_fd,&demod->requested_status,sizeof(demod->requested_status),0,(struct sockaddr *)&sdraddr,sizeof(sdraddr)) == -1)
    perror("sendto control socket");

  // Return the tuner's current frequency, which will change
  return demod->status.frequency;
}

// If avoid_alias is true, return 1 if specified carrier frequency is in range of LO2 given
// sampling rate, filter setting and alias region
//
// If avoid_alias is false, simply test that specified frequency is between +/- samplerate/2
int LO2_in_range(struct demod * const demod,double const f,int const avoid_alias){
  if(demod == NULL)
    return -1;

  // Wait until the sample rate is known
  pthread_mutex_lock(&demod->status_mutex);
  while(demod->samprate == 0)
    pthread_cond_wait(&demod->status_cond,&demod->status_mutex);
  pthread_mutex_unlock(&demod->status_mutex);
    
  if(avoid_alias)
    return f >= demod->min_IF + max(0.0f,demod->high)
	    && f <= demod->max_IF + min(0.0f,demod->low);
  else {
    return fabs(f) <=  0.5 * demod->samprate; // within Nyquist limit?
  }
}

// The next two frequency setting functions depend on the sample rate
// If it's not known (0) don't try to set the phasor step; let the input routine call us again

// Set second local oscillator (the one in software)
// the caller must avoid aliasing, e.g., with LO2_in_range()
double set_second_LO(struct demod * const demod,double const second_LO){
  if(demod == NULL)
    return NAN;

  if(!is_phasor_init(demod->second_LO_phasor)) // Initialized?
    demod->second_LO_phasor = 1;

  // When setting frequencies, assume TCXO also drives sample clock, so use same calibration
  // In case sample rate isn't set yet, just remember the frequency but don't divide by zero
  if(demod->samprate != 0)
    demod->second_LO_phasor_step = csincos(2*M_PI*second_LO/demod->samprate);

  demod->second_LO = second_LO;
  return second_LO;
}

// Set audio frequency shift after downconversion and detection (linear modes only: SSB, IQ, DSB)
double set_shift(struct demod * const demod,double const shift){
  demod->shift = shift;
  if(demod->samprate != 0)
    demod->shift_phasor_step = csincos(2*M_PI*shift*demod->decimate/demod->samprate);
  if(!is_phasor_init(demod->shift_phasor))
    demod->shift_phasor = 1;
  return shift;
}

// Set major operating mode
// This kills the current demodulator thread, sets up the predetection filter
// and other demodulator parameters, and starts the appropriate demodulator thread
int set_mode(struct demod * const demod,const char * const mode,int const defaults){
  if(demod == NULL)
    return -1;

  int mindex;
  for(mindex=0; mindex<Nmodes; mindex++){
    if(strcasecmp(mode,Modes[mindex].name) == 0)
      break;
  }
  if(mindex == Nmodes)
    return -1; // Unregistered mode

  // Kill current demod thread, if any, to cause clean exit
  demod->terminate = 1;
  pthread_join(demod->demod_thread,NULL); // Wait for it to finish
  demod->terminate = 0;

  // if the mode argument points to demod->mode, avoid the copy; can cause an abort
  if(demod->mode != mode)
    strlcpy(demod->mode,mode,sizeof(demod->mode));

  strlcpy(demod->demod_name, Modes[mindex].demod_name, sizeof(demod->demod_name));


  if(defaults || isnan(demod->low) || isnan(demod->high)){
    demod->low = Modes[mindex].low;
    demod->high = Modes[mindex].high;
  }

  // Ensure low < high
  if(demod->high < demod->low){
    float const tmp = demod->low;
    demod->low = demod->high;
    demod->high = tmp;
  }
  demod->flags = Modes[mindex].flags;
  demod->attack_rate = Modes[mindex].attack_rate;
  demod->recovery_rate = Modes[mindex].recovery_rate;
  demod->hangtime = Modes[mindex].hangtime;
  
  // Suppress these in display unless they're used
  demod->snr = NAN;
  demod->pdeviation = NAN;
  demod->cphase = NAN;
  demod->plfreq = NAN;
  demod->spare = NAN;

  if(defaults || isnan(demod->shift)){
    if(demod->shift != Modes[mindex].shift){
      // Adjust tuning for change in frequency shift
      set_freq(demod,get_freq(demod) + Modes[mindex].shift - demod->shift,NAN);
    }
    set_shift(demod,Modes[mindex].shift);
  }

  // Might now be out of range because of change in filter passband
  set_freq(demod,get_freq(demod),NAN);

  pthread_create(&demod->demod_thread,NULL,Modes[mindex].demod,demod);
  return 0;
}      


// Called from RTP receiver to process incoming metadata from SDR front end
void update_status(struct demod *demod,struct status *new_status){
      // Protect status with a mutex and signal a condition when it changes
      // since demod threads will be waiting for this
      int sig = 0;
      demod->status.timestamp = new_status->timestamp; // This should always change
      if(new_status->samprate != demod->status.samprate){
	// A/D sample rate is now known or has changed
	// This needs to be set before the demod thread starts!
	// Signalled every time the status is updated
	// status.samprate contains *nominal* A/D sample rate
	// Use nominal rates here so result is clean integer
	pthread_mutex_lock(&demod->status_mutex);
	demod->status.samprate = new_status->samprate;
	if(demod->status.samprate >= Audio.samprate){
	  // Sample rate is higher than audio rate; decimate
	  demod->interpolate = 1;
	  demod->decimate = demod->status.samprate / Audio.samprate;
	  demod->samprate = demod->status.samprate;
	  demod->max_IF = IF_EXCLUDE * demod->status.samprate/2;
	  demod->min_IF = -demod->max_IF;
	} else {
	  // Sample rate is lower than audio rate
	  // Interpolate up to audio rate, pretend sample rate is audio rate
	  demod->decimate = 1; 
	  demod->interpolate = Audio.samprate / demod->status.samprate;	  
	  demod->samprate = Audio.samprate;
	  demod->max_IF = IF_EXCLUDE * Audio.samprate/2;
	  demod->min_IF = -demod->max_IF;
	}
	// re-call these two to recalculate their phasor steps
	pthread_mutex_unlock(&demod->status_mutex);
	set_second_LO(demod,get_second_LO(demod));
	set_shift(demod,demod->shift);
	sig++;
      }
      // Gain settings changed? Store and signal but take no other action for now
      if(new_status->lna_gain != demod->status.lna_gain){
	demod->status.lna_gain = new_status->lna_gain;
	sig++;
      }
      if(new_status->mixer_gain != demod->status.mixer_gain){
	demod->status.mixer_gain = new_status->mixer_gain;
	sig++;
      }
      if(new_status->if_gain != demod->status.if_gain){
	demod->status.if_gain = new_status->if_gain;
	sig++;
      }
      if(new_status->frequency != demod->status.frequency){
	pthread_mutex_lock(&demod->status_mutex);
	// Tuner is now set or has been changed
	// Adjust 2nd LO to compensate
	// NB! This may take the 2nd LO out of its range. This is deliberate so we don't get
	// into fights over the SDR tuner. If the tuner comes back, we'll recover
	demod->status.frequency = new_status->frequency;
	pthread_mutex_unlock(&demod->status_mutex);
	double new_LO2 = -(demod->freq - get_first_LO(demod));
	set_second_LO(demod,new_LO2);
	sig++;
      }
      if(sig){
	// Voltage gain to normalize signal power for analog gain settings
	demod->gain_factor = powf(10.,-0.05*(demod->status.lna_gain + demod->status.if_gain + demod->status.mixer_gain));
	// Something changed, store the new status and let everybody know
	pthread_mutex_lock(&demod->status_mutex);
	pthread_cond_broadcast(&demod->status_cond);
	pthread_mutex_unlock(&demod->status_mutex);
      }
}

// Compute noise spectral density - experimental, my algorithm
// The problem is telling signal from noise
// Heuristic: first average all bins outside the bandwidth
// Then recompute the average, tossing bins > 3 dB above the previous average
// Hopefully this will get rid of any signals from the noise estimate
float const compute_n0(struct demod const * const demod){
  if(demod == NULL || demod->filter_in == NULL)
    return NAN;
  
  struct filter_in const *f = demod->filter_in;
  int const N = f->ilen + f->impulse_length - 1;
  float power_spectrum[N];
  
  // Compute smoothed power spectrum
  // There will be some spectral leakage because the convolution FFT we're using is unwindowed
  // Includes both real and imaginary components, so this will have to be divided by 2 to get 0dBFS convention
  for(int n=0;n<N;n++)
    power_spectrum[n] = cnrmf(f->fdomain[n]);

  // compute average energy outside passband, then iterate computing a new average that
  // omits bins > 3dB above the previous average. This should pick up only the noise
  float avg_n = INFINITY;
  for(int iter=0;iter<2;iter++){
    int noisebins = 0;
    float new_avg_n = 0;
    for(int n=0;n<N;n++){
      float f;
      if(n <= N/2)
	f = (float)(n * demod->samprate) / N;
      else
	f = (float)((n-N) * demod->samprate) / N;
      
      if(f >= demod->low && f <= demod->high)
	continue; // Avoid passband
      
      float const s = power_spectrum[n];
      if(s < avg_n * 2){ // +3dB threshold
	new_avg_n += s;
	noisebins++;
      }
    }
    new_avg_n /= noisebins;
    avg_n = new_avg_n;
  }
  // return noise power per Hz, normalized to 0dBFS
  return avg_n / (2*N*demod->samprate);
}
// Return 1 if complex phasor appears to be initialized, 0 if not
int is_phasor_init(const complex double x){
  if(isnan(creal(x)) || isnan(cimag(x)) || cnrm(x) < 0.9)
    return 0;
  return 1;
}
