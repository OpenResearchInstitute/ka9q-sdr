// $Id: osc.c,v 1.2 2018/12/05 09:07:18 karn Exp $
// Complex oscillator object routines

#define _GNU_SOURCE 1
#include <assert.h>
#include <math.h>
#include <complex.h>
#include "osc.h"
#include "dsp.h"

const int Renorm_rate = 16384; // Renormalize oscillators this often

// Return 1 if complex phasor appears to be initialized, 0 if not
int is_phasor_init(const complex double x){
  if(isnan(creal(x)) || isnan(cimag(x)) || cnrm(x) < 0.9)
    return 0;
  return 1;
}

// Set oscillator frequency and sweep rate
// Units are cycles/sample and cycles/sample^2
void set_osc(struct osc *osc,double f,double r){
  pthread_mutex_lock(&osc->mutex);
  if(!is_phasor_init(osc->phasor)){
    osc->phasor = 1; // Don't jump phase if already initialized
    osc->steps = 0;
  }
  osc->freq = f;
  osc->rate = r;
  osc->phasor_step = csincospi(2 * osc->freq);
  if(osc->rate != 0)
    osc->phasor_step_step = csincospi(2 * osc->rate);
  else
    osc->phasor_step_step = 1;
  pthread_mutex_unlock(&osc->mutex);
}

// Step oscillator through one sample, return complex phase
complex double step_osc(struct osc *osc){
  complex double r;

  r = osc->phasor;
  if(osc->freq != 0){
    osc->phasor *= osc->phasor_step;
    if(osc->rate != 0)
      osc->phasor_step *= osc->phasor_step_step;
  }
  if(++osc->steps == Renorm_rate)
    renorm_osc(osc);
  return r;
}

void renorm_osc(struct osc *osc){
  osc->steps = 0;
  osc->phasor /= cabs(osc->phasor);

  if(osc->rate != 0)
    osc->phasor_step /= cabs(osc->phasor_step);
}

