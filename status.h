#ifndef _STATUS_H
#define _STATUS_H 1
#include <stdint.h>


enum status_type {
  EOL = 0,	  
  GPS_TIME,       // Nanoseconds since GPS epoch (remember to update the leap second tables!)
  COMMANDS,       // Count of input commands
  INPUT_SOURCE_SOCKET,
  INPUT_DEST_SOCKET,
  INPUT_SSRC,
  INPUT_SAMPRATE, // Nominal sample rate (integer)
  INPUT_PACKETS,
  INPUT_SAMPLES,
  INPUT_DROPS,
  INPUT_DUPES,

  OUTPUT_DEST_SOCKET,
  OUTPUT_SSRC,
  OUTPUT_TTL,
  OUTPUT_SAMPRATE,
  OUTPUT_PACKETS,

  // Tuning
  RADIO_FREQUENCY,
  FIRST_LO_FREQUENCY,
  SECOND_LO_FREQUENCY,
  SHIFT_FREQUENCY,
  DOPPLER_FREQUENCY,
  DOPPLER_FREQUENCY_RATE,

  // Hardware
  CALIBRATE,
  LNA_GAIN,
  MIXER_GAIN,
  IF_GAIN,
  DC_I_OFFSET,
  DC_Q_OFFSET,
  IQ_IMBALANCE,
  IQ_PHASE,

  // Filtering
  LOW_EDGE,
  HIGH_EDGE,
  KAISER_BETA,
  FILTER_BLOCKSIZE,
  FILTER_FIR_LENGTH,
  NOISE_BANDWIDTH,

  // Signals
  IF_POWER,
  BASEBAND_POWER,
  NOISE_DENSITY,

  // Demodulation
  RADIO_MODE, // printable string "usb", "lsb", etc
  DEMOD_MODE, // 0 = linear (default), 1 = AM envelope, 2 = FM
  INDEPENDENT_SIDEBAND, // Linear only
  DEMOD_SNR,       // FM, PLL linear
  DEMOD_GAIN,      // AM, Linear
  FREQ_OFFSET,     // FM, PLL linear

  PEAK_DEVIATION, // FM only
  PL_TONE,        // FM only
  
  PLL_LOCK,       // Linear PLL
  PLL_SQUARE,     // Linear PLL
  PLL_PHASE,      // Linear PLL

  OUTPUT_CHANNELS, // 1 or 2 in Linear, otherwise 1
};

// Previous transmitted state, used to detect changes
struct state {
  int length;
  unsigned char value[256];
};


int encode_string(unsigned char **bp,enum status_type type,void *buf,int buflen);
int encode_eol(unsigned char **buf);
int encode_byte(unsigned char **buf,enum status_type type,unsigned char x);
int encode_int16(unsigned char **buf,enum status_type type,uint16_t x);
int encode_int32(unsigned char **buf,enum status_type type,uint32_t x);
int encode_int64(unsigned char **buf,enum status_type type,uint64_t x);
int encode_float(unsigned char **buf,enum status_type type,float x);
int encode_double(unsigned char **buf,enum status_type type,double x);

int compact_packet(struct state *s,unsigned char *pkt,int force);

uint64_t decode_int(unsigned char *,int);
float decode_float(unsigned char *,int);
double decode_double(unsigned char *,int);


#endif
