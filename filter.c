// $Id: filter.c,v 1.28 2018/02/06 11:45:57 karn Exp $
// General purpose filter package using fast convolution (overlap-save)
// and the FFTW3 FFT package
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
// Copyright 2017, Phil Karn, KA9Q, karn@ka9q.net

#define _GNU_SOURCE 1
#include <stdlib.h>
#include <pthread.h>
#include <memory.h>
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include "misc.h"
#include "filter.h"

// Create fast convolution filters
// The filters are now in two parts, filter_in (the master) and filter_out (the slave)
// Filter_in holds the original time-domain input and its frequency domain version
// Filter_out holds the frequency response and decimation information for one of several output filters that can share the same input

// filter_create_input() parameters, shared by all slaves:
// L = input data blocksize
// M = impulse response duration
// in_type = REAL or COMPLEX

// filter_create_output() parameters, distinct per slave
// master - pointer to associated master (input) filter
// response = complex frequency response; may be NULL here and set later with set_filter()
// This is set in the slave and can be different (indeed, this is the reason to have multiple slaves)
//            NB: response is always complex even when input and/or output is real, though it will be shorter
//            length = N_dec = (L + M - 1)/decimate when output is complex
//            length = (N_dec/2+1) when output is real
//            Must be SIMD-aligned (e.g., allocated with fftw_alloc) and will be freed by delete_filter()

// decimate = input/output sample rate ratio, only tested for powers of 2
// out_type = REAL, COMPLEX or CROSS_CONJ (COMPLEX with special processing for ISB)

// All demodulators taking baseband (zero IF) I/Q data require COMPLEX input
// All but SSB require COMPLEX output, with ISB using the special CROSS_CONJ mode
// SSB(CW) could (and did) use the REAL mode since the imaginary component is unneeded, and the c2r IFFT is faster
// Baseband FM audio filtering for de-emphasis and PL separation uses REAL input and output

// If you provide your own filter response, ensure that it drops to nil well below the Nyquist rate
// to prevent aliasing. Remember that decimation reduces the Nyquist rate by the decimation ratio.
// The set_filter() function uses Kaiser windowing for this purpose

// Set up input (master) half of filter
struct filter_in *create_filter_input(unsigned int const L,unsigned int const M, enum filtertype const in_type){

  int const N = L + M - 1;

  struct filter_in * const master = calloc(1,sizeof(*master));

  pthread_mutex_init(&master->filter_mutex,NULL);
  master->blocknum = 0;
  pthread_cond_init(&master->filter_cond,NULL);

  master->in_type = in_type;
  master->ilen = L;
  master->impulse_length = M;

  switch(in_type){
  default:
    fprintf(stderr,"Filter input type %d, assuming complex\n",in_type); // Note fall-thru
  case COMPLEX:
    master->fdomain = fftwf_alloc_complex(N);
    assert(master->fdomain != NULL);
    master->input_buffer.c = fftwf_alloc_complex(N);
    assert(malloc_usable_size(master->input_buffer.c) >= N * sizeof(*master->input_buffer.c));
    memset(master->input_buffer.c,0,(M-1)*sizeof(*master->input_buffer.c)); // Clear earlier state
    master->input.c = master->input_buffer.c + M - 1;
    master->fwd_plan = fftwf_plan_dft_1d(N,master->input_buffer.c,master->fdomain,FFTW_FORWARD,FFTW_ESTIMATE);
    break;
  case REAL:
    master->fdomain = fftwf_alloc_complex(N/2+1); // Only N/2+1 will be filled in by the r2c FFT
    assert(master->fdomain != NULL);
    master->input_buffer.r = fftwf_alloc_real(N);
    assert(malloc_usable_size(master->input_buffer.r) >= N * sizeof(*master->input_buffer.r));
    memset(master->input_buffer.r,0,(M-1)*sizeof(*master->input_buffer.r)); // Clear earlier state
    master->input.r = master->input_buffer.r + M - 1;
    master->fwd_plan = fftwf_plan_dft_r2c_1d(N,master->input_buffer.r,master->fdomain,FFTW_ESTIMATE);
    break;
  }
  return master;
}
// Set up output (slave) side of filter (possibly one of several sharing the same input master)

// Example: processing FM after demodulation to separate the PL tone and to de-emphasize the audio
// These output filters should be deleted before their masters
// Segfault will occur if filter_in is deleted and execute_filter_output is executed
struct filter_out *create_filter_output(struct filter_in * master,complex float * response,unsigned int decimate, enum filtertype out_type){
  assert(master != NULL);
  if(master == NULL)
    return NULL;

  int const N = master->ilen + master->impulse_length - 1;
  int const N_dec = N / decimate;

  // Parameter sanity check
  if((N % decimate) != 0)
    fprintf(stderr,"Warning: FFT size %'u is not divisible by decimation ratio %d\n",N,decimate);

  struct filter_out * const slave = calloc(1,sizeof(*slave));
  if(slave == NULL)
    return NULL;
  // Share all but decimation ratio, response, output and output type
  slave->master = master;
  slave->out_type = out_type;
  slave->decimate = decimate;
  slave->olen = master->ilen / decimate;
  slave->response = response;
  if(response != NULL)
    slave->noise_gain = noise_gain(slave);
  else
    slave->noise_gain = NAN;
  
  switch(slave->out_type){
  default:
  case COMPLEX:
  case CROSS_CONJ:
    slave->f_fdomain = fftwf_alloc_complex(N_dec);
    assert(slave->f_fdomain != NULL);
    slave->output_buffer.c = fftwf_alloc_complex(N_dec);
    assert(slave->output_buffer.c != NULL);
    slave->output.c = slave->output_buffer.c + N_dec - slave->olen;
    slave->rev_plan = fftwf_plan_dft_1d(N_dec,slave->f_fdomain,slave->output_buffer.c,FFTW_BACKWARD,FFTW_ESTIMATE);
    break;
  case REAL:
    slave->f_fdomain = fftwf_alloc_complex(N_dec/2+1);
    assert(slave->f_fdomain != NULL);    
    slave->output_buffer.r = fftwf_alloc_real(N_dec);
    assert(slave->output_buffer.r != NULL);
    //    slave->output.r = slave->output_buffer.r + (master->impulse_length - 1)/decimate;
    slave->output.r = slave->output_buffer.r + N_dec - slave->olen;
    slave->rev_plan = fftwf_plan_dft_c2r_1d(N_dec,slave->f_fdomain,slave->output_buffer.r,FFTW_ESTIMATE);
    break;
  }
  return slave;
}
int execute_filter_input(struct filter_in * const master){
  assert(master != NULL);
  if(master == NULL)
    return -1;

  fftwf_execute(master->fwd_plan);  // Forward transform

  // Notify slaves of new data
  pthread_mutex_lock(&master->filter_mutex);
  master->blocknum++;
  pthread_cond_broadcast(&master->filter_cond);
  pthread_mutex_unlock(&master->filter_mutex);

  // Perform overlap-and-save operation for fast convolution; note memmove is non-destructive
  switch(master->in_type){
  default:
  case COMPLEX:
    assert(malloc_usable_size(master->input_buffer.c) >= (master->ilen + master->impulse_length - 1)*sizeof(*master->input_buffer.c));
    memmove(master->input_buffer.c,master->input_buffer.c + master->ilen,(master->impulse_length - 1)*sizeof(*master->input_buffer.c));
    break;
  case REAL:
    assert(malloc_usable_size(master->input_buffer.r) >= (master->ilen + master->impulse_length - 1)*sizeof(*master->input_buffer.r));
    memmove(master->input_buffer.r,master->input_buffer.r + master->ilen,(master->impulse_length - 1)*sizeof(*master->input_buffer.r));
    break;
  }
  return 0;
}


int execute_filter_output(struct filter_out * const slave){
  assert(slave != NULL);
  if(slave == NULL)
    return -1;

  struct filter_in *master = slave->master;
  assert(master != NULL);
  assert(slave->out_type != NONE);
  assert(master->in_type != NONE);
  assert(master->fdomain != NULL);
  assert(slave->f_fdomain != NULL);  

  int const N = master->ilen + master->impulse_length - 1; // points in input buffer
  int const N_dec = N / slave->decimate;                     // points in (decimated) output buffer

  // DC and positive frequencies up to nyquist frequency are same for all types
  assert(malloc_usable_size(slave->f_fdomain) >= (N_dec/2+1) * sizeof(*slave->f_fdomain));
  assert(malloc_usable_size(master->fdomain) >= (N_dec/2+1) * sizeof(*master->fdomain));

  // Wait for new block of data
  pthread_mutex_lock(&master->filter_mutex);
  while(slave->blocknum == master->blocknum)
    pthread_cond_wait(&master->filter_cond,&master->filter_mutex);
  slave->blocknum = master->blocknum;
  pthread_mutex_unlock(&master->filter_mutex);

  pthread_mutex_lock(&slave->response_mutex); // Protect access to response[] array
  assert(malloc_usable_size(slave->response) >= (N_dec/2+1) * sizeof(*slave->response));
  assert(slave->response != NULL);

  // Positive frequencies up to half the nyquist rate are the same for all types
  for(int p=0; p <= N_dec/2; p++){
    slave->f_fdomain[p] = slave->response[p] * master->fdomain[p];
  }
  if(master->in_type == REAL){
    if(slave->out_type != REAL){
      // For a purely real input, F[-f] = conj(F[+f])
      assert(malloc_usable_size(slave->f_fdomain) >= N_dec * sizeof(*slave->f_fdomain));
      int p,dn;
      for(p=1,dn=N_dec-1; dn > N_dec/2; p++,dn--){
	slave->f_fdomain[dn] = slave->response[dn] * conjf(master->fdomain[p]);
      }
    } // out_type == REAL already handled
  } else { // in_type == COMPLEX
    if(slave->out_type != REAL){
      // Complex output; do negative frequencies
      assert(malloc_usable_size(master->fdomain) >= N * sizeof(*master->fdomain));
      assert(malloc_usable_size(slave->response) >= N_dec * sizeof(*slave->response));
      assert(malloc_usable_size(slave->f_fdomain) >= N_dec * sizeof(*slave->f_fdomain));

      for(int n=N-1,dn=N_dec-1; dn > N_dec/2;n--,dn--){
	slave->f_fdomain[dn] = slave->response[dn] * master->fdomain[n];
      }
    } else {
      // Real output; fold conjugates of negative frequencies into positive to force pure real result
      assert(malloc_usable_size(master->fdomain) >= N * sizeof(*master->fdomain));
      assert(malloc_usable_size(slave->response) >= N_dec * sizeof(*slave->response));
      for(int n=N-1,p=1,dn=N_dec-1; p < N_dec/2; p++,n--,dn--){
	slave->f_fdomain[p] += conjf(slave->response[dn] * master->fdomain[n]);
      }
    }
  }
  pthread_mutex_unlock(&slave->response_mutex); // release response[]

  if(slave->out_type == CROSS_CONJ){
    // hack for ISB; forces negative frequencies onto I, positive onto Q
    assert(malloc_usable_size(slave->f_fdomain) >= N_dec * sizeof(*slave->f_fdomain));
    for(int p=1,dn=N_dec-1; p < N_dec/2; p++,dn--){
      complex float const pos = slave->f_fdomain[p];
      complex float const neg = slave->f_fdomain[dn];
      
      slave->f_fdomain[p]  = pos + conjf(neg);
      slave->f_fdomain[dn] = neg - conjf(pos);
    }
  }
  fftwf_execute(slave->rev_plan); // Note: c2r version destroys f_fdomain[]
  return 0;
}

int delete_filter_input(struct filter_in * const master){
  if(master == NULL)
    return 0;
  
  fftwf_destroy_plan(master->fwd_plan);
  fftwf_free(master->input_buffer.c);
  fftwf_free(master->fdomain);
  free(master);
  return 0;
}
int delete_filter_output(struct filter_out * const slave){
  if(slave == NULL)
    return 0;
  
  pthread_mutex_destroy(&slave->response_mutex);
  fftwf_destroy_plan(slave->rev_plan);  
  fftwf_free(slave->output_buffer.c);
  fftwf_free(slave->response);
  fftwf_free(slave->f_fdomain);
  free(slave);
  return 0;
}

// Window shape factor for Kaiser window
// Transition region is approx sqrt(1+Beta^2)
float Kaiser_beta = 3.0;

// Modified Bessel function of the 0th kind, used by the Kaiser window
static const float i0(float const x){
  const float t = 0.25 * x * x;
  float sum = 1 + t;
  float term = t;
  for(int k=2;k<40;k++){
    term *= t/(k*k);
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return sum;
}


#if 0 // Available if you ever want them

// Hamming window
const static float hamming(int const n,int const M){
  const float alpha = 25./46;
  const float beta = (1-alpha);

  return alpha - beta * cos(2*M_PI*n/(M-1));
}

// Hann / "Hanning" window
const static float hann(int const n,int const M){
    return 0.5 - 0.5 * cos(2*M_PI*n/(M-1));
}

// Exact Blackman window
const static float blackman(int const n,int const M){
  float const a0 = 7938./18608;
  float const a1 = 9240./18608;
  float const a2 = 1430./18608;
  return a0 - a1*cos(2*M_PI*n/(M-1)) + a2*cos(4*M_PI*n/(M-1));
}

// Jim Kaiser was in my Bellcore department in the 1980s. Wonder whatever happened to him.
// Superseded by make_kaiser() routine that more efficiently computes entire window at once
static float const kaiser(int const n,int const M, float const beta){
  static float old_beta = NAN;
  static float old_inv_denom;

  // Cache old value of beta, since it rarely changes
  if(beta != old_beta){
    old_beta = beta;
    old_inv_denom = 1. / i0(M_PI*beta);
  }
  const float p = 2.0*n/(M-1) - 1;
  return i0(M_PI*beta*sqrtf(1-p*p)) * old_inv_denom;
}
#endif

// Compute an entire Kaiser window
// More efficient than repeatedly calling kaiser(n,M,beta)
int make_kaiser(float * const window,unsigned int const M,float const beta){
  assert(window != NULL);
  if(window == NULL)
    return -1;
  // Precompute unchanging partial values
  float const numc = M_PI * beta;
  float const inv_denom = 1. / i0(numc); // Inverse of denominator
  float const pc = 2.0 / (M-1);

  // The window is symmetrical, so compute only half of it and mirror
  // this won't compute the middle value in an odd-length sequence
  for(int n = 0; n < M/2; n++){
    float const p = pc * n  - 1;
    window[M-1-n] = window[n] = i0(numc * sqrtf(1-p*p)) * inv_denom;
  }
  // If sequence length is odd, middle value is unity
  if(M & 1)
    window[(M-1)/2] = 1; // The -1 is actually unnecessary

  return 0;
}


// Apply Kaiser window to filter frequency response
// "response" is SIMD-aligned array of N complex floats
// Impulse response will be limited to first M samples in the time domain
// Phase is adjusted so "time zero" (center of impulse response) is at M/2
// L and M refer to the decimated output
int window_filter(int const L,int const M,complex float * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;
  int const N = L + M - 1;
  assert(malloc_usable_size(response) >= N*sizeof(*response));
  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  complex float * const buffer = fftwf_alloc_complex(N);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD,FFTW_ESTIMATE);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD,FFTW_ESTIMATE);

  // Convert to time domain
  memcpy(buffer,response,N*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
  

  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);


  // Round trip through FFT/IFFT scales by N
  float const gain = 1./N;
  // Shift to beginning of buffer to make causal; apply window and gain
  for(int n = M - 1; n >= 0; n--)
    buffer[n] = buffer[(n-M/2+N)%N] * kaiser_window[n] * gain;
  // Pad with zeroes on right side
  memset(buffer+M,0,(N-M)*sizeof(*buffer));

#if 0
  fprintf(stderr,"Filter impulse response, shifted, windowed and zero padded\n");
  for(int n=0;n< N;n++)
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);

#if 0
  fprintf(stderr,"Filter response amplitude\n");
  for(int n=0;n<N;n++){
    float f = n*192000./N;
    fprintf(stderr,"%.1f %.1f\n",f,power2dB(cnrmf(buffer[n])));
  }
  fprintf(stderr,"\n");
#endif
  memcpy(response,buffer,N*sizeof(*response));
  fftwf_free(buffer);
  return 0;
}
// Real-only counterpart to window_filter()
// response[] is only N/2+1 elements containing DC and positive frequencies only
// Negative frequencies are inplicitly the conjugate of the positive frequencies
// L and M refer to the decimated output
int window_rfilter(int const L,int const M,complex float * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;
  int const N = L + M - 1;
  assert(malloc_usable_size(response) >= (N/2+1)*sizeof(*response));
  complex float * const buffer = fftwf_alloc_complex(N/2 + 1); // plan destroys its input
  assert(buffer != NULL);
  float * const timebuf = fftwf_alloc_real(N);
  assert(timebuf != NULL);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_r2c_1d(N,timebuf,buffer,FFTW_ESTIMATE);
  assert(fwd_filter_plan != NULL);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_c2r_1d(N,buffer,timebuf,FFTW_ESTIMATE);
  assert(rev_filter_plan != NULL);
  
  // Convert to time domain
  memcpy(buffer,response,(N/2+1)*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);

  // Shift to beginning of buffer, apply window and scale (N*N)
  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);
  // Round trip through FFT/IFFT scales by N
  float const gain = 1./N;
  for(int n = M - 1; n >= 0; n--)
    timebuf[n] = timebuf[(n-M/2+N)%N] * kaiser_window[n] * gain;
  
  // Pad with zeroes on right side
  memset(timebuf+M,0,(N-M)*sizeof(*timebuf));
#if 0
  printf("Filter impulse response, shifted, windowed and zero padded\n");
  for(int n=0;n< M;n++)
    printf("%d %lg\n",n,timebuf[n]);
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fftwf_free(timebuf);
#if 0
  printf("Filter frequency response\n");
  for(int n=0; n < N/2 + 1; n++)
    printf("%d %g %g (%.1f dB)\n",n,crealf(buffer[n]),cimagf(buffer[n]),
	   power2dB(cnrmf(buffer[n])));
#endif
  memcpy(response,buffer,(N/2+1)*sizeof(*response));
  fftwf_free(buffer);
  return 0;
}

// Gain of filter (output / input) on uniform gaussian noise
float const noise_gain(struct filter_out const * const filter){
  if(filter == NULL)
    return NAN;
  struct filter_in *master = filter->master;

  int const N = master->ilen + master->impulse_length - 1;
  int const N_dec = N / filter->decimate;

  float sum = 0;
  if(master->in_type == REAL && filter->out_type == REAL){
    for(int i=0;i<N_dec/2+1;i++)
      sum += cnrmf(filter->response[i]);
  } else {
    for(int i=0;i<N_dec;i++)
      sum += cnrmf(filter->response[i]);
  }
  // the factor N compensates for the unity gain scaling
  // Amplitude is pre-scaled 1/N for the concatenated (FFT/IFFT) round trip, so the overall power
  // is scaled 1/N^2. Multiplying by N gives us correct power in the frequency domain (just the FFT)

  // The factor of 2 undoes the 1/sqrt(2) amplitude scaling required for unity signal gain in these two modes
  if(filter->out_type == REAL || filter->out_type == CROSS_CONJ)
    return 2*N*sum;
  else
    return N*sum;
}


int set_filter(struct filter_out * const slave,float const dsamprate,float const low,float const high,float const kaiser_beta){
  assert(slave != NULL);
  if(slave == NULL)
    return -1;
  struct filter_in *master = slave->master;


  int const L_dec = slave->olen;
  int const M_dec = (master->impulse_length - 1) / slave->decimate + 1;
  int const N_dec = L_dec + M_dec - 1;
  int const N = master->ilen + master->impulse_length - 1;

  float gain = 1./((float)N);
#if 1
  if(slave->out_type == REAL || slave->out_type == CROSS_CONJ)
    gain *= M_SQRT1_2;
#endif

  complex float * const response = fftwf_alloc_complex(N_dec);
  for(int n=0;n<N_dec;n++){
    float f;
    if(n <= N_dec/2)
      f = (float)n * dsamprate / N_dec;
    else
      f = (float)(n-N_dec) * dsamprate / N_dec;
    if(f >= low && f <= high)
      response[n] = gain;
    else
      response[n] = 0;
  }
  window_filter(L_dec,M_dec,response,kaiser_beta);
  // Hot swap with existing response, if any, using mutual exclusion
  pthread_mutex_lock(&slave->response_mutex);
  complex float *tmp = slave->response;
  slave->response = response;
  slave->noise_gain = noise_gain(slave);
  pthread_mutex_unlock(&slave->response_mutex);
  fftwf_free(tmp);

  return 0;
}


// Experimental IIR complex notch filter

struct notchfilter *notch_create(double const f,float const bw){
  struct notchfilter *nf = calloc(1,sizeof(struct notchfilter));
  if(nf == NULL)
    return NULL;

  nf->osc_phase = 1;
  nf->osc_step = csincos(2*M_PI*f);
  nf->dcstate = 0;
  nf->bw = bw;
  return nf;
}

complex float notch(struct notchfilter * const nf,complex float s){
  if(nf == NULL)
    return NAN;
  s = s * conj(nf->osc_phase) - nf->dcstate; // Spin down and remove DC
  nf->dcstate += nf->bw * s;   // Update smoothed estimate
  s *= nf->osc_phase;          // Spin back up
  nf->osc_phase *= nf->osc_step;
  return s;
}
