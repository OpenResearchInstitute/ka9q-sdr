// $Id: sdr.h,v 1.6 2018/04/09 21:09:27 karn Exp $
#ifndef _SDR_H
#define _SDR_H 1

#include <stdint.h>
#include <complex.h>
#undef I

int mirics_gain(double f,int gr,uint8_t *bb, uint8_t *lna,uint8_t *mix);
int front_end_init(int,int,int);
int get_adc(short *,int);
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
