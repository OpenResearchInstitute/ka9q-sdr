// $Id: iqplay.c,v 1.24 2018/04/22 21:46:05 karn Exp $
// Read from IQ recording, multicast in (hopefully) real time
// Copyright 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <locale.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>

#include "misc.h"
#include "radio.h"
#include "multicast.h"
#include "attr.h"


int Verbose;
double Default_frequency = 0;
long Default_samprate = 192000;
int Blocksize = 256;

int Rtp_sock; // Socket handle for sending real time stream

// Play I/Q file with descriptor 'fd' on network socket 'sock'
int playfile(int sock,int fd,int blocksize){
  struct status status;
  memset(&status,0,sizeof(status));
  status.samprate = Default_samprate; // Not sure this is useful
  status.frequency = Default_frequency;
  attrscanf(fd,"samplerate","%ld",&status.samprate);
  attrscanf(fd,"frequency","%lf",&status.frequency);
  if(attrscanf(fd,"source_timestamp","%lld",&status.timestamp) == -1){
    double unixstarttime;
    attrscanf(fd,"unixstarttime","%lf",&unixstarttime);
    // Convert decimal seconds from UNIX epoch to integer nanoseconds from GPS epoch
    status.timestamp = (unixstarttime  - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000000LL;
  }
  if(Verbose)
    fprintf(stderr,": start time %s, %'d samp/s, RF LO %'.1lf Hz\n",lltime(status.timestamp),status.samprate,status.frequency);

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = IQ_PT;         // ordinarily dynamically allocated
  
  struct timeval start_time;
  gettimeofday(&start_time,NULL);
  rtp.ssrc = start_time.tv_sec;
  int timestamp = 0;
  int seq = 0;
  
  // microsec between packets. Double precision is used to avoid small errors that could
  // accumulate over time
  double dt = (1000000. * blocksize) / status.samprate;
  // Microseconds since start for next scheduled transmission; will transmit first immediately
  double sked_time = 0;
  
  while(1){
    rtp.seq = seq++;
    rtp.timestamp = timestamp;
    timestamp += blocksize;
    
    // Is it time yet?
    while(1){
      // Microseconds since start
      struct timeval tv,diff;
      gettimeofday(&tv,NULL);
      timersub(&tv,&start_time,&diff);
      double rt = 1000000. * diff.tv_sec + diff.tv_usec;
      if(rt >= sked_time)
	break;
      if(sked_time > rt + 100){
	// Use care here, s is unsigned
	useconds_t s = (sked_time - rt) - 100; // sleep until 100 microseconds before
	usleep(s);
      }
    }
    unsigned char output_buffer[4*blocksize + 256]; // will this allow for largest possible RTP header??
    unsigned char *dp = output_buffer;
    dp = hton_rtp(dp,&rtp);
    dp = hton_status(dp,&status);

    if(pipefill(fd,dp,4*blocksize) <= 0)
      break;

    dp += 4*blocksize;

    int length = dp - output_buffer;
    if(send(sock,output_buffer,length,0) == -1)
      perror("send");
    
    // Update time of next scheduled transmission
    sked_time += dt;
    // Update nanosecond timestamp
    status.timestamp += blocksize * (long long)1e9 / status.samprate;
  }
  return 0;
}



int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  seteuid(getuid());

  char *dest = "iq.playback.mcast.local"; // Default for testing
  char *locale = getenv("LANG");

  Mcast_ttl = 1; // By default, don't let it route
  int c;
  while((c = getopt(argc,argv,"vl:b:R:f:r:T:")) != EOF){
    switch(c){
    case 'R':
      dest = optarg;
      break;
    case 'r':
      Default_samprate = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
    case 'l':
      locale = optarg;
      break;
    case 'b':
      Blocksize = strtol(optarg,NULL,0);
      break;
    case 'f': // Used only if there's no tag on a file, or for stdin
      Default_frequency = strtod(optarg,NULL);
      break;
    }
  }
  if(argc < optind){
    fprintf(stderr,"Usage: %s [options] [filename]\n",argv[0]);
    exit(1);
  }

  setlocale(LC_ALL,locale);
  // Set up RTP output socket
  Rtp_sock = setup_mcast(dest,1);

  signal(SIGPIPE,SIG_IGN);

  if(optind == argc){
    // No file arguments, read from stdin
    if(Verbose)
      fprintf(stderr,"Transmitting from stdin");
    playfile(Rtp_sock,0,Blocksize);
  } else {
    for(int i=optind;i<argc;i++){
      int fd;
      if((fd = open(argv[i],O_RDONLY)) == -1){
	fprintf(stderr,"Can't read %s; ",argv[i]);
	perror("");
	continue;
      }
      if(Verbose)
	fprintf(stderr,"Transmitting %s",argv[i]);
      playfile(Rtp_sock,fd,Blocksize);
      close(fd);
      fd = -1;
    }
  }
  close(Rtp_sock);
  Rtp_sock = -1;
  exit(0);
}

