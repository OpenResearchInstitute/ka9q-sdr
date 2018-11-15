// $Id: audio.c,v 1.77 2018/11/14 23:09:29 karn Exp $
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
int send_stereo_audio(struct audio * const audio,float const * buffer,int size){

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.type = PCM_STEREO_PT;         // 16 bit linear, big endian, stereo
  rtp.version = RTP_VERS;
  rtp.ssrc = audio->rtp.ssrc;

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
    rtp.timestamp = audio->rtp.timestamp;
    audio->rtp.timestamp += chunk/2; // Increase by sample count
    if(not_silent){
      audio->rtp.bytes += sizeof(signed short) * chunk;
      audio->rtp.packets++;
      if(audio->silent){
	audio->silent = 0;
	rtp.marker = 1;
      } else
	rtp.marker = 0;
      rtp.seq = audio->rtp.seq++;
      unsigned char packet[PACKETSIZE],*dp;
      dp = packet;

      dp = hton_rtp(dp,&rtp);
      memcpy(dp,PCM_buf,2*chunk);
      dp += 2*chunk;
      int r = send(audio->audio_mcast_fd,&packet,dp - packet,0);
      if(r < 0){
	perror("pcm: send");
	break;
      }
    } else
      audio->silent = 1;
    size -= chunk/2;
  }
  return 0;
}

// Send 'size' mono samples, each in a float
int send_mono_audio(struct audio * const audio,float const * buffer,int size){

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = PCM_MONO_PT;         // 16 bit linear, big endian, mono
  rtp.ssrc = audio->rtp.ssrc;

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
    rtp.timestamp = audio->rtp.timestamp;
    audio->rtp.timestamp += chunk; // Increase by sample count
    if(not_silent){
      // Don't send silence, but timestamp is still incremented
      audio->rtp.packets++;
      audio->rtp.bytes += sizeof(signed short) * chunk;
      if(audio->silent){
	audio->silent = 0;
	rtp.marker = 1;
      } else
	rtp.marker = 0;
      rtp.seq = audio->rtp.seq++;
      unsigned char packet[PACKETSIZE];
      unsigned char *dp = packet;

      dp = hton_rtp(dp,&rtp);
      memcpy(dp,PCM_buf,2*chunk);
      dp += 2 * chunk;

      int r = send(audio->audio_mcast_fd,&packet,dp - packet,0);
      if(r < 0){
	perror("pcm: send");
	break;
      }
    } else
      audio->silent = 1;
    size -= chunk;
  }
  return 0;
}

void audio_cleanup(void *p){
  struct audio * const audio = p;
  if(audio == NULL)
    return;

  if(audio->audio_mcast_fd > 0){
    close(audio->audio_mcast_fd);
    audio->audio_mcast_fd = -1;
  }
}

// Set up for PCM audio output
int setup_audio(struct audio * const audio,int ttl){
  assert(audio != NULL);

  // If not already set, Use time of day as RTP SSRC
  if(audio->rtp.ssrc == 0){
    time_t tt = time(NULL);
    audio->rtp.ssrc = tt & 0xffffffff;
  }
  audio->audio_mcast_fd = setup_mcast(audio->audio_mcast_address_text,1,ttl,0);
  if(audio->audio_mcast_fd == -1)
    return -1;
  audio->rtcp_mcast_fd = setup_mcast(audio->audio_mcast_address_text,1,ttl,1);
  if(audio->rtcp_mcast_fd == -1)
    return -1;

  return 0;
}
