// $Id: misc.c,v 1.24 2018/04/22 22:25:40 karn Exp $
// Miscellaneous low-level DSP routines
// Copyright 2018, Phil Karn, KA9Q

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 // Needed to get sincos/sincosf
#endif
#include <complex.h>
#undef I
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "misc.h"
#include "radio.h"

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

// Return 1 if complex phasor appears to be initialized, 0 if not
int is_phasor_init(const complex double x){
  if(isnan(creal(x)) || isnan(cimag(x)) || cnrm(x) < 0.9)
    return 0;
  return 1;
}


// Fill buffer from pipe
// Needed because reads from a pipe can be partial
int pipefill(const int fd,void *buffer,const int cnt){
  int i;
  unsigned char *bp = buffer;
  for(i=0;i<cnt;){
    int n = read(fd,bp+i,cnt-i);
    if(n < 0)
      return n;
    if(n == 0)
      break;
    i += n;
  }
  return i;

}

// Remove return or newline, if any, from end of string
void chomp(char *s){

  if(s == NULL)
    return;
  char *cp;
  if((cp = strchr(s,'\r')) != NULL)
    *cp = '\0';
  if((cp = strchr(s,'\n')) != NULL)
    *cp = '\0';
}


// Parse a frequency entry in the form
// 12345 (12345 Hz)
// 12k345 (12.345 kHz)
// 12m345 (12.345 MHz)
// 12g345 (12.345 GHz)
// If no g/m/k and number is too small, make a heuristic guess
// NB! This assumes radio covers 100 kHz - 2 GHz; should make more general
double const parse_frequency(const char *s){
  char * const ss = alloca(strlen(s));

  int i;
  for(i=0;i<strlen(s);i++)
    ss[i] = tolower(s[i]);

  ss[i] = '\0';
  
  // k, m or g in place of decimal point indicates scaling by 1k, 1M or 1G
  char *sp;
  double mult;
  if((sp = strchr(ss,'g')) != NULL){
    mult = 1e9;
    *sp = '.';
  } else if((sp = strchr(ss,'m')) != NULL){
    mult = 1e6;
    *sp = '.';
  } else if((sp = strchr(ss,'k')) != NULL){
    mult = 1e3;
    *sp = '.';
  } else
    mult = 1;

  char *endptr = NULL;
  double f = strtod(ss,&endptr);
  if(endptr == ss || f == 0)
    return 0; // Empty entry, or nothing decipherable
  
  if(mult != 1 || f >= 1e5) // If multiplier given, or frequency >= 100 kHz (lower limit), return as-is
    return f * mult;
    
  // If frequency would be out of range, guess kHz or MHz
  if(f < 100)
    f *= 1e6;              // 0.1 - 99.999 Only MHz can be valid
  else if(f < 500)         // Could be kHz or MHz, arbitrarily assume MHz
    f *= 1e6;
  else if(f < 2000)        // Could be kHz or MHz, arbitarily assume kHz
    f *= 1e3;
  else if(f < 100000)      // Can only be kHz
    f *= 1e3;

  return f;
}

char *Days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
char *Months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };

// Format a timestamp expressed as nanoseconds from the GPS epoch
char *lltime(long long t){
  struct tm tm;
  time_t utime;
  int t_usec;
  static char result[100];

  utime = (t / 1000000000LL) - GPS_UTC_OFFSET + UNIX_EPOCH;
  t_usec = (t % 1000000000LL) / 1000;
  if(t_usec < 0){
    t_usec += 1000000;
    utime -= 1;
  }


  gmtime_r(&utime,&tm);
  // Mon Feb 26 14:40:08.123456 UTC 2018
  snprintf(result,sizeof(result),"%s %s %d %02d:%02d:%02d.%06d UTC %4d",
	   Days[tm.tm_wday],Months[tm.tm_mon],tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,t_usec,tm.tm_year+1900);
  return result;

}
