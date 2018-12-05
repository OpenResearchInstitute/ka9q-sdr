// $Id: filter.h,v 1.17 2018/11/27 07:31:26 karn Exp $
// General purpose filter package using fast convolution (overlap-save)
// and the FFTW3 FFT package
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
// Copyright 2017, Phil Karn, KA9Q, karn@ka9q.net
#ifndef _FILTER_H
#define _FILTER_H 1

#include <complex.h>
#include <fftw3.h>
#undef I

// Input can be REAL or COMPLEX
// Output can be REAL, COMPLEX or CROSS_CONJ, i.e., COMPLEX with special cross conjugation for ISB
enum filtertype {
  NONE,
  COMPLEX,
  CROSS_CONJ,
  REAL,
};

// Input and output arrays can be either complex or real
union rc {
  float *r;
  complex float *c;
};

struct filter {
  enum filtertype in_type;           // REAL or COMPLEX
  enum filtertype out_type;          // REAL, COMPLEX or CROSS_CONJ
  unsigned int ilen;                          // Length of user portion of input buffer, aka 'L'
  unsigned int impulse_length;                // Length of filter impulse response, aka 'M'
  complex float *response;           // Filter response in frequency domain
  complex float *fdomain;            // Signal in frequency domain
  complex float *f_fdomain;          // Filtered signal in frequency domain
  float noise_gain;                  // Filter gain on uniform noise (ratio < 1)
  union rc input_buffer;             // Actual time-domain input buffer, length N = L + M - 1
  union rc input;                    // Beginning of user input area, length L
  union rc output_buffer;            // Actual time-domain output buffer, length N/decimate
  union rc output;                   // Beginning of user output area, length L/decimate
  fftwf_plan fwd_plan;               // FFT (time -> frequency)
  fftwf_plan rev_plan;               // IFFT (frequency -> time)
  unsigned int decimate;                      // Ratio of input to output sample rate
  unsigned int olen;                          // Length of user portion of output buffer
  int slave;                         // Filter is a slave to another
  pthread_mutex_t response_mutex;             // Protect the response array during dynamic modification
  pthread_mutex_t filter_mutex;
  int filter_blocknum;
  pthread_cond_t filter_cond;

};
struct filter_in {
  enum filtertype in_type;           // REAL or COMPLEX
  unsigned int ilen;                          // Length of user portion of input buffer, aka 'L'
  unsigned int impulse_length;                // Length of filter impulse response, aka 'M'
  complex float *fdomain;            // Signal in frequency domain
  union rc input_buffer;             // Actual time-domain input buffer, length N = L + M - 1
  union rc input;                    // Beginning of user input area, length L
  fftwf_plan fwd_plan;               // FFT (time -> frequency)
  unsigned int blocknum;                      // Data sequence number, used to notify slaves of new data
  pthread_mutex_t filter_mutex;      // Synchronization for sequence number
  pthread_cond_t filter_cond;

};
struct filter_out {
  struct filter_in *master;
  enum filtertype out_type;          // REAL, COMPLEX or CROSS_CONJ
  complex float *response;           // Filter response in frequency domain
  pthread_mutex_t response_mutex;
  complex float *f_fdomain;          // Filtered signal in frequency domain
  float noise_gain;                  // Filter gain on uniform noise (ratio < 1)
  union rc output_buffer;            // Actual time-domain output buffer, length N/decimate
  union rc output;                   // Beginning of user output area, length L/decimate
  fftwf_plan rev_plan;               // IFFT (frequency -> time)
  unsigned int decimate;                      // Ratio of input to output sample rate
  unsigned int olen;                          // Length of user portion of output buffer
  unsigned int blocknum;                      // Last sequence number received from master, used for synchronization
};
int window_filter(int L,int M,complex float *response,float beta);
int window_rfilter(int L,int M,complex float *response,float beta);

struct filter_in *create_filter_input(unsigned int const L,unsigned int const M, enum filtertype const in_type);
struct filter_out *create_filter_output(struct filter_in * master,complex float * response,unsigned int decimate, enum filtertype out_type);
int execute_filter_input(struct filter_in *);
int execute_filter_output(struct filter_out *);
int delete_filter_input(struct filter_in *);
int delete_filter_output(struct filter_out *);
int make_kaiser(float *window,unsigned int M,float beta);
int set_filter(struct filter_out *,float,float,float);
float const noise_gain(struct filter_out const *);


// Experimental complex notch filter
struct notchfilter {
  complex double osc_phase; // Phase of local complex mixer
  complex double osc_step;  // mixer phase increment (frequency)
  complex float dcstate;    // Average signal at mixer frequency
  float bw;                 // Relative bandwidth of notch
};

struct notchfilter *notch_create(double,float);
#define notch_delete(x) free(x)
complex float notch(struct notchfilter *,complex float);

#endif
