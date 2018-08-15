// $Id: dsp.h,v 1.18 2018/07/06 06:10:44 karn Exp $
// Low-level, ostly math functions useful for digital signal processing
#ifndef _DSP_H
#define _DSP_H 1

#include <complex.h>
#undef I

#include <math.h> // Get M_PI

#define M_1_2PI (0.5 * M_1_PI) // fraction of a rotation in one radian

double const angle_mod(double);

float const fast_atan2f(float,float);
inline float const fast_cargf(complex float x){
  return fast_atan2f(cimagf(x),crealf(x));
}

complex float const csincosf(const float x);
complex double const csincos(const double x);

float const rpower(const float *data,const int len);
float const cpower(const complex float *data, const int len);
float complex const cpowers(complex float const *,const int);

// Compute norm = x * conj(x) = Re{x}^2 + Im{x}^2
float const cnrmf(const complex float x);
double const cnrm(const complex double x);

double const parse_frequency(const char *);

#define dB2power(x) (powf(10.,(x)/10.))
#define power2dB(x) (10*log10f(x))
#define dB2voltage(x) (powf(10.,(x)/20.))
#define voltage2dB(x) (20*log10f(x))

#define DEGPRA (180./M_PI)
#define RAPDEG (M_PI/180.)

// I *hate* this sort of pointless, stupid, gratuitous incompatibility that
// makes a lot of code impossible to read and debug
#ifdef __APPLE__
#define sincos(x,s,c) {*s = sin(x); *c = cos(x);}
#endif


#endif
