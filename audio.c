// $Id: audio.c,v 1.67 2018/04/11 07:08:18 karn Exp $
// Audio multicast routines for KA9Q SDR receiver
// Handles linear 16-bit PCM, mono and stereo
// Copyright 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "misc.h"
#include "audio.h"
#include "multicast.h"

#define PCM_BUFSIZE 480        // 16-bit word count; must fit in Ethernet MTU

uint16_t Rtp_seq = 0;
uint32_t Ssrc;
uint32_t Timestamp;



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
  rtp.ssrc = Ssrc;

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
    rtp.timestamp = Timestamp;
    Timestamp += chunk/2; // Increase by sample count
    if(not_silent){
      audio->audio_packets++;
      if(audio->silent){
	audio->silent = 0;
	rtp.marker = 1;
      } else
	rtp.marker = 0;
      rtp.seq = Rtp_seq++;
      unsigned char packet[2048],*dp;
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
  rtp.ssrc = Ssrc;

  int16_t PCM_buf[PCM_BUFSIZE];

  while(size > 0){
    int not_silent = 0;
    int chunk = min(PCM_BUFSIZE,size); // # of mono samples (frames)
    for(int i=0; i < chunk; i++){
      float samp = *buffer++;
      PCM_buf[i] = htons(scaleclip(samp));
      not_silent |= PCM_buf[i];
    }      
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = Timestamp;
    Timestamp += chunk; // Increase by sample count
    if(not_silent){
      // Don't send silence, but timestamp is still incremented
      audio->audio_packets++;
      if(audio->silent){
	audio->silent = 0;
	rtp.marker = 1;
      } else
	rtp.marker = 0;
      rtp.seq = Rtp_seq++;
      unsigned char packet[2048];
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

// Set up encoding/sending tasks
int setup_audio(struct audio * const audio,int blocksize){
  assert(audio != NULL);

  time_t tt = time(NULL);
  Ssrc = tt & 0xffffffff;

  audio->audio_mcast_fd = setup_mcast(Audio.audio_mcast_address_text,1);

  return 0;
}
