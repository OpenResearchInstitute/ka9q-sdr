// $Id: fm.c,v 1.55 2018/10/12 00:20:25 karn Exp $
// FM demodulation and squelch
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#undef I

#include "misc.h"
#include "dsp.h"
#include "filter.h"
#include "radio.h"

void *pltask(void *); // Measure PL tone frequency

// FM demodulator thread
void *demod_fm(void *arg){
  pthread_setname("fm");
  assert(arg != NULL);
  struct demod * const demod = arg;  
  struct audio * const audio = &Audio; // Eventually pass as argument

  complex float state = 1; // Arbitrary choice; zero would cause first audio sample to be NAN
  float const dsamprate = demod->samprate / demod->decimate; // Decimated (output) sample rate
  demod->pdeviation = 0;
  demod->foffset = 0;
  demod->flags |= MONO; // Only mono for now
  demod->gain = NAN; // We don't use this, turn it off on the display

  // Create predetection filter, leaving response momentarily empty
  struct filter_out * const filter = create_filter_output(demod->filter_in,NULL,demod->decimate,COMPLEX);
  demod->filter_out = filter;
  set_filter(filter,dsamprate,demod->low,demod->high,demod->kaiser_beta);

  // Set up audio baseband filter master
  // Can have two slave filters: one for the de-emphasized audio output, another for the PL tone measurement
  int const AL = demod->L / demod->decimate;
  int const AM = (demod->M - 1) / demod->decimate + 1;
  int const AN = AL + AM - 1;
  float const filter_gain = 10./AN;  // Add some gain to bring up subjective volume level
  struct filter_in *audio_master = create_filter_input(AL,AM,REAL);

  demod->audio_master = audio_master;

  // Thread running 10-second FFT at low sample rate to measure PL tone frequency
  pthread_t pl_thread;
  pthread_create(&pl_thread,NULL,pltask,demod);

  // Voice filter, unless FLAT mode is selected
  // Audio response is high pass with 300 Hz corner to remove PL tone
  // then -6 dB/octave post-emphasis since demod is FM and modulation is actually PM (indirect FM)
  struct filter_out *audio_filter = NULL;
  if(!(demod->flags & FLAT)){
    complex float * const aresponse = fftwf_alloc_complex(AN/2+1);
    assert(aresponse != NULL);
    memset(aresponse,0,(AN/2+1) * sizeof(*aresponse));
    for(int j=0;j<=AN/2;j++){
      float const f = (float)j * dsamprate / AN;
      if(f >= 300 && f <= 6000)
	aresponse[j] = filter_gain * 300./f; // -6 dB/octave de-emphasis to handle PM (indirect FM) transmission
    }
    // Window scaling for REAL input, REAL output
    window_rfilter(AL,AM,aresponse,demod->kaiser_beta);
    audio_filter = create_filter_output(audio_master,aresponse,1,REAL); // Real input, real output, same sample rate
  }
  
  float lastaudio = 0; // state for impulse noise removal
  int snr_below_threshold = 0; // Number of blocks in which FM snr is below threshold, used for squelch

  while(!demod->terminate){

    // Wait for next block of frequency domain data
    execute_filter_output(filter);
    // Compute bb_power below along with average amplitude to save time
    //    demod->bb_power = cpower(filter->output.c,filter->olen);
    float const n0 = compute_n0(demod);
    if(isnan(demod->n0))
      demod->n0 = n0; // handle startup transient
    else
      demod->n0 += .01 * (n0 - demod->n0);

    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    float const gain = (demod->headroom *  M_1_PI * dsamprate) / fabsf(demod->low - demod->high);

    // Find average amplitude and estimate SNR for squelch
    // We also need average magnitude^2, but we have that from demod->bb_power
    // Approximate for SNR because magnitude has a chi-squared distribution with 2 degrees of freedom
    float avg_amp = 0;
    demod->bb_power = 0;
    for(int n=0;n<filter->olen;n++){
      float const t = cnrmf(filter->output.c[n]);
      demod->bb_power += t;
      avg_amp += sqrtf(t);        // magnitude
    }
    // Scale to each component so baseband power display is correct
    demod->bb_power /= 2 * filter->olen;
    avg_amp /= M_SQRT2 * filter->olen;         // Average magnitude
    float const fm_variance = demod->bb_power - avg_amp*avg_amp;
    demod->snr = avg_amp*avg_amp/(2*fm_variance) - 1;
    demod->snr = max(0.0f,demod->snr); // Smoothed values can be a little inconsistent

    // Demodulated FM samples
    float samples[audio_master->ilen];
    // Start timer when SNR falls below threshold
    int const thresh = 2;
    if(demod->snr > thresh) { // +3dB? +6dB?
      snr_below_threshold = 0;
    } else {
      if(++snr_below_threshold > 1000)
	snr_below_threshold = 1000; // Could conceivably wrap if squelch is closed for long time
    }
    if(snr_below_threshold < 2){ // Squelch is (still) open
      // keep the squelch open an extra block to flush out the filters and buffers

      // Threshold extension by comparing sample amplitude to threshold
      // 0.55 is empirical constant, 0.5 to 0.6 seems to sound good
      // Square amplitudes are compared to avoid sqrt inside loop
      float const min_ampl = 0.55 * 0.55 * avg_amp * avg_amp;
      //     float const min_ampl = 0; // turn off experimentally

      // Actual FM demodulation
      float pdev_pos = 0;
      float pdev_neg = 0;
      float avg_f = 0;
      for(int n=0; n<filter->olen; n++){
	complex float const samp = filter->output.c[n];
	if(cnrmf(samp) > min_ampl){ // Blank weak samples
	  lastaudio = samples[n] = audio_master->input.r[n] = cargf(samp * state); // Phase change from last sample
	  state = conjf(samp);
	  // Track of peak deviation only if signal is present
	  if(n == 0)
	    pdev_pos = pdev_neg = lastaudio;
	  else if(lastaudio > pdev_pos)
	    pdev_pos = lastaudio;
	  else if(lastaudio < pdev_neg)
	    pdev_neg = lastaudio;
	} else {
	  samples[n] = audio_master->input.r[n] = lastaudio; // Replace unreliable sample with last good one
	}
	avg_f += lastaudio;
      }
      avg_f /= filter->olen;  // Average FM output is freq offset
      if(snr_below_threshold < 1){
	// Squelch open; update frequency offset and peak deviation
	demod->foffset = dsamprate  * avg_f * M_1_2PI;

	// Remove frequency offset from deviation peaks and scale
	pdev_pos -= avg_f;
	pdev_neg -= avg_f;
	demod->pdeviation = dsamprate * max(pdev_pos,-pdev_neg) * M_1_2PI;
      }
    } else {
      state = 0;
      lastaudio = 0;
      // Squelch is closed, send zeroes for a little while longer
      memset(samples,0,audio_master->ilen * sizeof(*samples));
      memset(audio_master->input.r,0,audio_master->ilen*sizeof(*audio_master->input.r));
    }
    execute_filter_input(audio_master); // Pass to post-detection audio filter(s)

    if(audio_filter != NULL){
      execute_filter_output(audio_filter);

      // in FM flat mode there is no audio filter, and audio is already in samples[]
      assert(audio_master->ilen == audio_filter->olen);
      for(int n=0; n < audio_filter->olen; n++)
	samples[n] = audio_filter->output.r[n] * gain;

    }
    send_mono_audio(audio,samples,audio_master->ilen);
  }
  // Clean up subthreads
  pthread_join(pl_thread,NULL);


  if(audio_filter != NULL)
    delete_filter_output(audio_filter); // Must delete first
  delete_filter_input(audio_master);
  delete_filter_output(filter);
  demod->filter_out = NULL;

  // Clear these to keep them from showing up with other demods
  demod->foffset = NAN;
  demod->pdeviation = NAN;
  demod->plfreq = NAN;
  demod->flags = 0;

  pthread_exit(NULL);
}

// pltask to measure PL tone frequency with FFT
void *pltask(void *arg){
  pthread_setname("pl");
  struct demod *demod = (struct demod *)arg;

  assert(demod != NULL);

  // N, L and sample rate for audio master filter (usually 48 kHz)
  int const AN = (demod->L + demod->M - 1) / demod->decimate;
  int const AL = demod->L / demod->decimate;
  float const dsamprate = demod->samprate / demod->decimate; // sample rate from FM demodulator

  // Pl slave filter parameters
  int const PL_decimate = 32; // 48 kHz in, 1500 Hz out
  float const PL_samprate = dsamprate / PL_decimate;
  int const PL_N = AN / PL_decimate;
  int const PL_L = AL / PL_decimate;
  int const PL_M = PL_N - PL_L + 1;

  // Low pass filter with 300 Hz cut to pass only PL tones
  complex float * const plresponse = fftwf_alloc_complex(PL_N/2+1);
  assert(plresponse != NULL);
  float const filter_gain = 1;
  memset(plresponse,0,(PL_N/2+1)*sizeof(*plresponse));
  // Positive frequencies only
  for(int j=0;j<=PL_N/2;j++){
    float const f = (float)j * dsamprate / AN; // frequencies are relative to INPUT sampling rate
    if(f > 0 && f < 300)
      plresponse[j] = filter_gain;
  } 
  window_rfilter(PL_L,PL_M,plresponse,2.0); // What's the optimum Kaiser window beta here?
  struct filter_out * const pl_filter = create_filter_output(demod->audio_master,plresponse,PL_decimate,REAL);

  // Set up long FFT to which we feed the PL tone for frequency analysis
  // PL analyzer sample rate = 48 kHz / 32 = 1500 Hz
  // FFT blocksize = 512k / 32 = 16k
  // i.e., one FFT buffer every 16k / 1500 = 10.92 sec, which gives < 0.1 Hz resolution
  int const pl_fft_size = (1 << 19) / PL_decimate;
  float * const pl_input = fftwf_alloc_real(pl_fft_size);
  complex float * const pl_spectrum = fftwf_alloc_complex(pl_fft_size/2+1);
  fftwf_plan pl_plan = fftwf_plan_dft_r2c_1d(pl_fft_size,pl_input,pl_spectrum,FFTW_ESTIMATE);
  assert(pl_plan != NULL);

  int fft_ptr = 0;
  int last_fft = 0;
  while(!demod->terminate){
    execute_filter_output(pl_filter);
 
    // Determine PL tone frequency with a long FFT operating at the low PL filter sample rate
    int remain = pl_filter->olen;
    last_fft += remain;
    float *data = pl_filter->output.r;
    while(remain != 0){
      int chunk = min(remain,pl_fft_size - fft_ptr);
      assert(malloc_usable_size(pl_input) >= sizeof(*pl_input) * (fft_ptr + chunk));
      memcpy(pl_input+fft_ptr,data,sizeof(*data) * chunk);
      fft_ptr += chunk;
      data += chunk;
      remain -= chunk;
      if(fft_ptr >= pl_fft_size)
	fft_ptr -= pl_fft_size;
    }
    // Execute only periodically
    if(last_fft >= 512){ // 512 / 1500 Hz = 0.34 seconds
      last_fft = 0;

      // Determine PL tone, if any
      fftwf_execute(pl_plan);
      int peakbin = -1;      // Index of peak energy bin
      float peakenergy = 0;  // Energy in peak bin
      float totenergy = 0;   // Total energy, all bins
      assert(malloc_usable_size(pl_spectrum) >= pl_fft_size/2 * sizeof(complex float));
      for(int n=1;n<pl_fft_size/2;n++){ // skip DC
	float const energy = cnrmf(pl_spectrum[n]);
	totenergy += energy;
	if(energy > peakenergy){
	  peakenergy = energy;
	  peakbin = n;
	}
      }
      // Standard PL tones range from 67.0 to 254.1 Hz; ignore out of range results
      // as they can be falsed by voice in the absence of a tone
      // Give a result only if the energy in the tone exceeds an arbitrary fraction of the total
      if(peakbin > 0 && peakenergy > 0.01 * totenergy){
	float const f = (float)peakbin * PL_samprate / pl_fft_size;
	if(f > 67 && f < 255)
	  demod->plfreq = f;
      } else
	demod->plfreq = NAN;
    }
  }
  // Clean up
  delete_filter_output(pl_filter);
  fftwf_destroy_plan(pl_plan);
  fftwf_free(pl_input);
  fftwf_free(pl_spectrum);
  return NULL;
}
