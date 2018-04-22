// $Id: audio.h,v 1.31 2018/04/04 01:55:31 karn Exp $
// Variables and structures for KA9Q SDR receiver audio subsystem
// Copyright 2017 Phil Karn, KA9Q

#ifndef _AUDIO_H
#define _AUDIO_H 1

#include <sys/types.h>
//#include <sys/socket.h>
//#include <netinet/in.h>
//#include <arpa/inet.h>

struct audio {
  int samprate;       // Audio D/A sample rate (usually decimated from SDR A/D)

  // RTP network streaming
  int silent; // last packet was suppressed (used to generate RTP mark bit)
  char audio_mcast_address_text[256];
  int audio_mcast_fd; // File descriptor for multicast output
  unsigned long long audio_packets;
};

extern struct audio Audio;

int send_mono_audio(struct audio *,const float *,int);
int send_stereo_audio(struct audio *,const float *,int);
int setup_audio(struct audio *,int);
void audio_cleanup(void *);

extern int DAC_samprate;
extern int Verbose;


#endif


