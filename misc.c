// $Id: misc.c,v 1.26 2018/07/06 06:08:45 karn Exp $
// Miscellaneous low-level routines, mostly time-related
// Copyright 2018, Phil Karn, KA9Q

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "misc.h"


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
