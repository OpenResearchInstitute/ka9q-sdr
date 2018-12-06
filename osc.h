#ifndef _OSC_H
#define _OSC_H 1

#define _GNU_SOURCE 1
#include <pthread.h>
#include <math.h>
#include <complex.h>

struct osc {
  double freq;
  double rate;
  complex double phasor;
  complex double phasor_step;
  complex double phasor_step_step;
  pthread_mutex_t mutex;
  int steps; // Steps since last normalize
};
#endif

// Osc functions
void set_osc(struct osc *osc,double f,double r);
complex double step_osc(struct osc *osc);
void renorm_osc(struct osc *osc);
int is_phasor_init(const complex double x);
