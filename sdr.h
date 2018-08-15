// $Id: sdr.h,v 1.8 2018/07/06 06:13:36 karn Exp $
// Interfaces to the Software Defined Radio front end hardware
// Right now these are somewhat specific to the AMSAT UK Funcube Dongle, and they
// need to be generalized to other hardware without losing the specific features
// of each device, e.g., the specific analog gain parameters
// Copyright 2018, Phil Karn KA9Q
#ifndef _SDR_H
#define _SDR_H 1

#include <stdint.h>

void closedown(int);

// Sent in each RTP packet right after header
// NB! because we just copy this into the network stream, it's important that the compiler
// not add any extra padding.
// To avoid this, the size must be a multiple of 8, the size of a double and long long
struct status {
  long long timestamp; // Nanoseconds since GPS epoch 6 Jan 1980 00:00:00 UTC
  double frequency;
  uint32_t samprate;
  uint8_t lna_gain;
  uint8_t mixer_gain;
  uint8_t if_gain;
  uint8_t unused; // pad to 16 bytes
};


static inline unsigned char *ntoh_status(struct status *status,unsigned char *data){
  // Host byte order
  status->timestamp = *(long long *)data;
  status->frequency = *(double *)&data[8];
  status->samprate = *(uint32_t *)&data[16];
  status->lna_gain = data[20];
  status->mixer_gain = data[21];
  status->if_gain = data[22];
  return data + 24;
}
static inline unsigned char *hton_status(unsigned char *data,struct status *status){
  // Host byte order
  *(long long *)data = status->timestamp;
  *(double *)&data[8] = status->frequency;
  *(uint32_t *)&data[16] = status->samprate;
  data[20] = status->lna_gain;
  data[21] = status->mixer_gain;
  data[22] = status->if_gain;
  return data + 24;
}



#endif
