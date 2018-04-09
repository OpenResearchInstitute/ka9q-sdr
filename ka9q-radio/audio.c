// $Id: audio.c,v 1.65 2018/04/04 14:17:51 karn Exp $
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
  rtp.mpt = PCM_STEREO_PT;         // 16 bit linear, big endian, stereo
  rtp.vpxcc = (RTP_VERS << 6); // Version 2, padding = 0, extension = 0, csrc count = 0
  rtp.ssrc = htonl(Ssrc);

  int16_t PCM_buf[PCM_BUFSIZE];

  struct iovec iovec[2];      
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = PCM_buf;
  
  struct msghdr message;      
  message.msg_name = NULL; // Set by connect() call in setup_output()
  message.msg_namelen = 0;
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;
  

  while(size > 0){
    int not_silent = 0;
    int chunk = min(PCM_BUFSIZE,2*size);
    for(int i=0; i < chunk; i ++){
      float samp = *buffer++;
      PCM_buf[i] = htons(scaleclip(samp));
      not_silent |= PCM_buf[i];
    }      
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = htonl(Timestamp);
    Timestamp += chunk/2; // Increase by sample count
    if(not_silent){
      audio->audio_packets++;
      if(audio->silent){
	audio->silent = 0;
	rtp.mpt |= RTP_MARKER;
      } else
	rtp.mpt &= ~RTP_MARKER;
      rtp.seq = htons(Rtp_seq++);
      iovec[1].iov_len = chunk * 2;
      int r = sendmsg(audio->audio_mcast_fd,&message,0);
      if(r < 0){
	perror("pcm: sendmsg");
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
  rtp.mpt = PCM_MONO_PT;         // 16 bit linear, big endian, mono
  rtp.vpxcc = (RTP_VERS << 6); // Version 2, padding = 0, extension = 0, csrc count = 0
  rtp.ssrc = htonl(Ssrc);

  int16_t PCM_buf[PCM_BUFSIZE];

  struct iovec iovec[2];      
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = PCM_buf;
  
  struct msghdr message;      
  message.msg_name = NULL; // Set by connect() call in setup_output()
  message.msg_namelen = 0;
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;
  

  while(size > 0){
    int not_silent = 0;
    int chunk = min(PCM_BUFSIZE,size); // # of mono samples (frames)
    for(int i=0; i < chunk; i++){
      float samp = *buffer++;
      PCM_buf[i] = htons(scaleclip(samp));
      not_silent |= PCM_buf[i];
    }      
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = htonl(Timestamp);
    Timestamp += chunk; // Increase by stereo sample count
    if(not_silent){
      // Don't send silence, but timestamp is still incremented
      audio->audio_packets++;
      if(audio->silent){
	audio->silent = 0;
	rtp.mpt |= RTP_MARKER;
      } else
	rtp.mpt &= ~RTP_MARKER;
      rtp.seq = htons(Rtp_seq++);
      iovec[1].iov_len = chunk * 2;
      int r = sendmsg(audio->audio_mcast_fd,&message,0);
      if(r < 0){
	perror("pcm: sendmsg");
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
