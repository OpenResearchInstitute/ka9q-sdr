// $Id: misc.c,v 1.27 2018/08/04 22:18:49 karn Exp $
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
#if __APPLE__

// OSX doesn't have pthread_barrier_*
// Taken from https://stackoverflow.com/questions/3640853/performance-test-sem-t-v-s-dispatch-semaphore-t-and-pthread-once-t-v-s-dispat
// Apparently by David Cairns

#include <errno.h>

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
    if(count == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(pthread_mutex_init(&barrier->mutex, 0) < 0)
    {
        return -1;
    }
    if(pthread_cond_init(&barrier->cond, 0) < 0)
    {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }
    barrier->tripCount = count;
    barrier->count = 0;

    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    ++(barrier->count);
    if(barrier->count >= barrier->tripCount)
    {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &(barrier->mutex));
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

#endif // __APPLE__

