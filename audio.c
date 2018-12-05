// $Id: audio.c,v 1.79 2018/12/02 09:16:45 karn Exp $
// Audio multicast routines for KA9Q SDR receiver
// Handles linear 16-bit PCM, mono and stereo
// Copyright 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "misc.h"
#include "multicast.h"
#include "radio.h"

#define PCM_BUFSIZE 480        // 16-bit word count; must fit in Ethernet MTU
#define PACKETSIZE 2048        // Somewhat larger than Ethernet MTU

static short const scaleclip(float const x){
  if(x >= 1.0)
    return SHRT_MAX;
  else if(x <= -1.0)
    return SHRT_MIN;
  return (short)(SHRT_MAX * x);
}
  

// Send 'size' stereo samples, each in a pair of floats
int send_stereo_output(struct demod * const demod,float const * buffer,int size){

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.type = PCM_STEREO_PT;         // 16 bit linear, big endian, stereo
  rtp.version = RTP_VERS;
  rtp.ssrc = demod->output.rtp.ssrc;

  int16_t PCM_buf[PCM_BUFSIZE];

  while(size > 0){
    int not_silent = 0;
    int chunk = min(PCM_BUFSIZE,2*size);

    for(int i=0; i < chunk; i ++){
      float samp = *buffer++;
      PCM_buf[i] = htons(scaleclip(samp));
      not_silent |= PCM_buf[i];
    }      
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = demod->output.rtp.timestamp;
    demod->output.rtp.timestamp += chunk/2; // Increase by sample count
    if(not_silent){
      demod->output.rtp.bytes += sizeof(signed short) * chunk;
      demod->output.rtp.packets++;
      if(demod->output.silent){
	demod->output.silent = 0;
	rtp.marker = 1;
      } else
	rtp.marker = 0;
      rtp.seq = demod->output.rtp.seq++;
      unsigned char packet[PACKETSIZE],*dp;
      dp = packet;

      dp = hton_rtp(dp,&rtp);
      memcpy(dp,PCM_buf,2*chunk);
      dp += 2*chunk;
      int r = send(demod->output.fd,&packet,dp - packet,0);
      if(r < 0){
	perror("pcm: send");
	break;
      }
    } else
      demod->output.silent = 1;
    size -= chunk/2;
  }
  return 0;
}

// Send 'size' mono samples, each in a float
int send_mono_output(struct demod * const demod,float const * buffer,int size){

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = PCM_MONO_PT;         // 16 bit linear, big endian, mono
  rtp.ssrc = demod->output.rtp.ssrc;

  int16_t PCM_buf[PCM_BUFSIZE];

  while(size > 0){
    int not_silent = 0;
    int chunk = min(PCM_BUFSIZE,size); // # of mono samples (frames)

    for(int i=0; i < chunk; i++){
      float samp = *buffer++;
      PCM_buf[i] = htons(scaleclip(samp));
      not_silent |= PCM_buf[i];
    }      
    //    not_silent = 1; // Disable silence-squelching !!!!!
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = demod->output.rtp.timestamp;
    demod->output.rtp.timestamp += chunk; // Increase by sample count
    if(not_silent){
      // Don't send silence, but timestamp is still incremented
      demod->output.rtp.packets++;
      demod->output.rtp.bytes += sizeof(signed short) * chunk;
      if(demod->output.silent){
	demod->output.silent = 0;
	rtp.marker = 1;
      } else
	rtp.marker = 0;
      rtp.seq = demod->output.rtp.seq++;
      unsigned char packet[PACKETSIZE];
      unsigned char *dp = packet;

      dp = hton_rtp(dp,&rtp);
      memcpy(dp,PCM_buf,2*chunk);
      dp += 2 * chunk;

      int r = send(demod->output.fd,&packet,dp - packet,0);
      if(r < 0){
	perror("pcm: send");
	break;
      }
    } else
      demod->output.silent = 1;
    size -= chunk;
  }
  return 0;
}

void output_cleanup(void *p){
  struct demod * const demod = p;
  if(demod == NULL)
    return;

  if(demod->output.fd > 0){
    close(demod->output.fd);
    demod->output.fd = -1;
  }
}

// Set up for PCM demod output
int setup_output(struct demod * const demod,int ttl){
  assert(demod != NULL);

  // If not already set, Use time of day as RTP SSRC
  if(demod->output.rtp.ssrc == 0){
    time_t tt = time(NULL);
    demod->output.rtp.ssrc = tt & 0xffffffff;
  }
  demod->output.fd = setup_mcast(demod->output.dest_address_text,(struct sockaddr *)&demod->output.dest_address,1,ttl,0);
  if(demod->output.fd == -1)
    return -1;
  socklen_t len = sizeof(demod->output.source_address);
  getsockname(demod->output.fd,(struct sockaddr *)&demod->output.source_address,&len);

  demod->output.rtcp_fd = setup_mcast(demod->output.dest_address_text,NULL,1,ttl,1);
  if(demod->output.rtcp_fd == -1)
    return -1;

  demod->output.status_fd = setup_mcast(demod->output.dest_address_text,NULL,1,ttl,2);
  if(demod->output.status_fd == -1)
    return -1;

  return 0;
}
