// $Id: dsp.c,v 1.1 2018/07/06 06:10:44 karn Exp $
// low-level subroutines useful in digital signal processing - mainly math related
// Copyright 2018, Phil Karn, KA9Q

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 // Needed to get sincos/sincosf
#endif


#include <complex.h>
#include <math.h>
#include "dsp.h"

// return unit magnitude complex number with phase x radians
// I.e., cos(x) + j*sin(x)
complex float const csincosf(const float x){
  float s,c;

#if __APPLE__ // No sincos
  s = sinf(x);
  c = cosf(x);
#else
  sincosf(x,&s,&c);
#endif
  return CMPLXF(c,s);
}

// return unit magnitude complex number with given phase x
complex double const csincos(const double x){
  double s,c;

  sincos(x,&s,&c);
  return CMPLX(c,s);
}

// Complex norm (sum of squares of real and imaginary parts)
float const cnrmf(const complex float x){
  return crealf(x)*crealf(x) + cimagf(x) * cimagf(x);
}
double const cnrm(const complex double x){
  return creal(x)*creal(x) + cimag(x) * cimag(x);
}
