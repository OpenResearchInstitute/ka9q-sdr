// $Id: linear.c,v 1.25 2018/08/09 03:55:46 karn Exp $

// General purpose linear demodulator
// Handles USB/IQ/CW/etc, basically all modes but FM and envelope-detected AM
// Copyright Sept 20 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <pthread.h>

#include "misc.h"
#include "dsp.h"
#include "filter.h"
#include "radio.h"


void *demod_linear(void *arg){
  pthread_setname("linear");
  assert(arg != NULL);
  struct demod * const demod = arg;
  struct audio * const audio = &Audio; // Eventually pass as argument

  demod->loop_bw = 1; // eventually to be set from mode table

  // Set derived (and other) constants
  float const samptime = demod->decimate / demod->samprate;  // Time between (decimated) samples
  float const blocktime = samptime * demod->L; // Update rate of fine PLL (once/block)

  // AGC
  int hangcount = 0;
  float const recovery_factor = dB2voltage(demod->recovery_rate * samptime); // AGC ramp-up rate/sample
#if 0
  float const attack_factor = dB2voltage(demod->attack_rate * samptime);      // AGC ramp-down rate/sample
#endif
  int const hangmax = demod->hangtime / samptime; // samples before AGC increase
  if(isnan(demod->gain))
    demod->gain = dB2voltage(100.0); // initial setting

  // Coherent mode parameters
  float const snrthreshdb = 3;     // Loop lock threshold at +3 dB SNR
  int   const fftsize = 1 << 16;   // search FFT bin size = 64K = 1.37 sec @ 48 kHz
  float const damping = M_SQRT1_2; // PLL loop damping factor; 1/sqrt(2) is "critical" damping
  float const lock_time = 1;       // hysteresis parameter: 2*locktime seconds of good signal -> lock, 2*locktime sec of bad signal -> unlock
  int   const fft_enable = 1;

  // FFT search params
  float const snrthresh = powf(10,snrthreshdb/10);          // SNR threshold for lock
  int   const lock_limit = round(lock_time / samptime);            // Stop sweeping after locked for this amount of time
  float const binsize = 1. / (fftsize * samptime);          // FFT bin size, Hz
  // FFT bin indices for search limits. Squaring doubles frequency, so double the search range
  float const searchhigh = 300;    // FFT search limits, in Hz
  float const searchlow =  -300;
  int   const lowlimit =  round(((demod->flags & SQUARE) ? 2 : 1) * searchlow / binsize);
  int   const highlimit = round(((demod->flags & SQUARE) ? 2 : 1) * searchhigh / binsize);

  // Second-order PLL loop filter (see Gardner)
  float const phase_scale = 2 * M_PI * samptime;           // radians/sample
  float const vcogain = 2*M_PI;                            // 1 Hz = 2pi radians/sec per "volt"
  float const pdgain = 1;                                  // phase detector gain "volts" per radian (unity from atan2)
  float const natfreq = demod->loop_bw * 2*M_PI;                  // loop natural frequency in rad/sec
  float const tau1 = vcogain * pdgain / (natfreq*natfreq); // 1 / 2pi
  float const integrator_gain = 1 / tau1;                  // 2pi
  float const tau2 = 2 * damping / natfreq;                // sqrt(2) / 2pi = 1/ (pi*sqrt(2))
  float const prop_gain = tau2 / tau1;                     // sqrt(2)/2
  //  float const ramprate = demod->loop_bw * blocktime / integrator_gain;   // sweep at one loop bw/sec
  float const ramprate = 0; // temp disable

#if 0
  // DC removal from envelope-detected AM and coherent AM
  complex float DC_filter = 0;
  float const DC_filter_coeff = .0001;
#endif

  demod->snr = 0;

  // Detection filter
  struct filter_out * const filter = create_filter_output(demod->filter_in,NULL,demod->decimate,
					       (demod->flags & ISB) ? CROSS_CONJ : COMPLEX);
  demod->filter_out = filter;
  set_filter(filter,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);

  // Carrier search FFT
  complex float * fftinbuf = NULL;
  complex float *fftoutbuf = NULL;
  fftwf_plan fft_plan = NULL;
  int fft_ptr = 0;  

  if(fft_enable){
    fftinbuf = fftwf_alloc_complex(fftsize);
    fftoutbuf = fftwf_alloc_complex(fftsize);  
    fft_plan = fftwf_plan_dft_1d(fftsize,fftinbuf,fftoutbuf,FFTW_FORWARD,FFTW_ESTIMATE);
  }

  // Initialize PLL
  complex float fine_phasor = 1;        // fine offset LO, controlled by PLL
  complex float fine_phasor_step = 1;
  complex float coarse_phasor = 1;      // FFT-controlled offset LO
  complex float coarse_phasor_step = 1; // 0 Hz to start
  float integrator = 0;                 // 2nd order loop integrator
  float delta_f = 0;                    // FFT-derived offset
  float ramp = 0;                       // Frequency sweep (do we still need this?)
  int lock_count = 0;
  int pll_lock = 0;

  while(!demod->terminate){
    // New samples
    // Copy ISB flag to filter, since it might change
    if(demod->flags & ISB)
      filter->out_type = CROSS_CONJ;
    else
      filter->out_type = COMPLEX;

    execute_filter_output(filter);    
    if(!isnan(demod->n0))
      demod->n0 += .001 * (compute_n0(demod) - demod->n0);
    else
      demod->n0 = compute_n0(demod); // Happens at startup

    // Carrier (or regenerated carrier) tracking in coherent mode
    if(demod->flags & PLL){
      // Copy into circular input buffer for FFT in case we need it for acquisition
      if(fft_enable){
	if(demod->flags & SQUARE){
	  // Squaring loop is enabled; square samples to strip BPSK or DSB modulation
	  // and form a carrier component at 2x its actual frequency
	  // This is of course suboptimal for BPSK since there's no matched filter,
	  // but it may be useful in a pinch
	  for(int i=0;i<filter->olen;i++){
	    fftinbuf[fft_ptr++] = filter->output.c[i] * filter->output.c[i];
	    if(fft_ptr >= fftsize)
	      fft_ptr -= fftsize;
	  }
	} else {
	  // No squaring, just analyze the samples directly for a carrier
	  for(int i=0;i<filter->olen;i++){
	    fftinbuf[fft_ptr++] = filter->output.c[i];
	    if(fft_ptr >= fftsize)
	      fft_ptr -= fftsize;
	  }
	}
      }
      // Loop lock detector with hysteresis
      // If the loop is locked, the SNR must fall below the threshold for a while
      // before we declare it unlocked, and vice versa
      if(demod->snr < snrthresh){
	lock_count -= filter->olen;
      } else {
	lock_count += filter->olen;
      }
      if(lock_count >= lock_limit){
	lock_count = lock_limit;
	pll_lock = 1;
      }
      if(lock_count <= -lock_limit){
	lock_count = -lock_limit;
	pll_lock = 0;
      }
      demod->spare = lock_count;

      // If loop is out of lock, reacquire
      if(!pll_lock){
	if(fft_enable){
	  // Run FFT, look for peak bin
	  // Do this every time??
	  fftwf_execute(fft_plan);
	  
	  // Search limited range of FFT buffer for peak energy
	  int maxbin = 0;
	  float maxenergy = 0;
	  for(int n = lowlimit; n <= highlimit; n++){
	    float const e = cnrmf(fftoutbuf[n < 0 ? n + fftsize : n]);
	    if(e > maxenergy){
	      maxenergy = e;
	      maxbin = n;
	    }
	  }
	  double new_delta_f = binsize * maxbin;
	  if(demod->flags & SQUARE)
	    new_delta_f /= 2; // Squaring loop provides 2xf component, so we must divide by 2
	  
	  if(new_delta_f != delta_f){
	    delta_f = new_delta_f;
	    integrator = 0; // reset integrator
	    coarse_phasor_step = csincos(-phase_scale * delta_f);
	  }
	}
	if(ramp == 0) // not already sweeping
	  ramp = ramprate;
      } else { // !pll_lock
	ramp = 0;
      }
      // Apply coarse and fine offsets, gather DC phase information
      complex float accum = 0;
      for(int n=0;n<filter->olen;n++){
	filter->output.c[n] *= coarse_phasor * fine_phasor;
	coarse_phasor *= coarse_phasor_step;
	fine_phasor *= fine_phasor_step;

	complex float ss = filter->output.c[n];
	if(demod->flags & SQUARE)
	  ss *= ss;
	
	accum += ss;
      }
      // Renormalize complex phasors
      fine_phasor /= cabs(fine_phasor);
      coarse_phasor /= cabs(coarse_phasor);

      demod->cphase = cargf(accum);
      if(demod->flags & SQUARE)
	demod->cphase /= 2; // Squaring doubles the phase


      // fine PLL on block basis
      // Includes ramp generator for frequency sweeping during acquisition
      float carrier_phase = demod->cphase;

      // Lag-lead (integral plus proportional) 
      integrator += carrier_phase * blocktime + ramp;
      float const feedback = integrator_gain * integrator + prop_gain * carrier_phase; // units of Hz
      if(fabsf(feedback * phase_scale) < .01)
	fine_phasor_step = CMPLXF(1,-phase_scale * feedback);  // Small angle approximation
      else
	fine_phasor_step = csincosf(-phase_scale * feedback); 
      
      // Acquisition frequency sweep
      if((feedback >= binsize) && (ramp > 0))
	ramp = -ramprate; // reached upward sweep limit, sweep down
      else if((feedback <= binsize) && (ramp < 0))
	ramp = ramprate;  // Reached downward sweep limit, sweep up
      
      if(isnan(demod->foffset))
	demod->foffset = feedback + delta_f;
      else
	demod->foffset += 0.001 * (feedback + delta_f - demod->foffset);
    } else {
      // Not used in non-coherent (i.e., non-PLL) modes
      demod->cphase = NAN;
      demod->foffset = NAN;
      demod->spare = NAN;
    }
    // Demodulation
    float signal = 0;
    float noise = 0;
    
    for(int n=0; n<filter->olen; n++){
      // Assume signal on I channel, so only noise on Q channel
      // True only in coherent modes when locked, but we'll need total power anyway
      signal += crealf(filter->output.c[n]) * crealf(filter->output.c[n]);
      noise += cimagf(filter->output.c[n]) * cimagf(filter->output.c[n]);
      float amplitude = cabsf(filter->output.c[n]);
      
      // AGC
      // Lots of people seem to have strong opinions how AGCs should work
      // so there's probably a lot of work to do here
      // The attack_factor feature doesn't seem to work well; if it's at all
      // slow you get an annoying "pumping" effect.
      // But if it's too fast, brief spikes can deafen you for some time
      // What to do?
      if(isnan(demod->gain)){
	demod->gain = demod->headroom / amplitude; // Startup
      } else if(amplitude * demod->gain > demod->headroom){
	demod->gain = demod->headroom / amplitude;
	//	  demod->gain *= attack_factor;
	hangcount = hangmax;
      } else if(hangcount != 0){
	hangcount--;
      } else {
	demod->gain *= recovery_factor;
      }
      filter->output.c[n] *= demod->gain;
    }
    // Optional frequency shift *after* demodulation and AGC
    pthread_mutex_lock(&demod->shift_mutex);
    if(demod->shift != 0){
      for(int n=0; n < filter->olen; n++){
	filter->output.c[n] *= demod->shift_phasor;
	demod->shift_phasor *= demod->shift_phasor_step;
      }
      demod->shift_phasor /= cabs(demod->shift_phasor);
    }
    pthread_mutex_unlock(&demod->shift_mutex);
    
    if(demod->flags & MONO) {
      // Send only I channel as mono
      float samples[filter->olen];
      for(int n=0; n<filter->olen; n++)
	samples[n] = crealf(filter->output.c[n]);
      send_mono_audio(audio,samples,filter->olen);
    } else {
      // I on left, Q on right
      send_stereo_audio(audio,(float *)filter->output.c,filter->olen);
    }
    // Total baseband power (I+Q), scaled to each sample
    demod->bb_power = (signal + noise) / (2*filter->olen);
    // PLL loop SNR, if used
    if(noise != 0 && (demod->flags & PLL)){
      demod->snr = (signal / noise) - 1; // S/N as power ratio; meaningful only in coherent modes
      if(demod->snr < 0)
	demod->snr = 0; // Clamp to 0 so it'll show as -Inf dB
    } else
      demod->snr = NAN;

  } // terminate
  if(fftinbuf)
    fftwf_free(fftinbuf);
  if(fftoutbuf)
    fftwf_free(fftoutbuf);  
  if(fft_plan)
    fftwf_destroy_plan(fft_plan);
  if(filter)
    delete_filter_output(filter);
  demod->filter_out = NULL;
  pthread_exit(NULL);
}
