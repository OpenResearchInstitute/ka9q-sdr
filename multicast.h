// $Id: multicast.h,v 1.21 2018/09/08 06:06:21 karn Exp $
// Multicast and RTP functions, constants and structures
// Not every RTP module uses these yet, they need to be revised
// Copyright 2018, Phil Karn, KA9Q

#ifndef _MULTICAST_H
#define _MULTICAST_H 1
#include <stdint.h>
#include <assert.h>

#define NTP_EPOCH 2208988800UL // Seconds between Jan 1 1900 and Jan 1 1970

#define RTP_MIN_SIZE 12  // min size of RTP header
#define RTP_VERS 2
#define RTP_MARKER 0x80  // Marker flag in mpt field

#define IQ_PT (97)    // NON-standard payload type for my raw I/Q stream - 16 bit version
#define IQ_PT8 (98)   // NON-standard payload type for my raw I/Q stream - 8 bit version
#define AX25_PT (96)  // NON-standard paylaod type for my raw AX.25 frames
#define PCM_MONO_PT (11)
#define PCM_STEREO_PT (10)
#define OPUS_PT (111) // Hard-coded NON-standard payload type for OPUS (should be dynamic with sdp)

// Internal representation of RTP header -- NOT what's on wire!
struct rtp_header {
  int version;
  uint8_t type;
  uint16_t seq;
  uint32_t timestamp;
  uint32_t ssrc;
  int marker:1;
  int pad:1;
  int extension:1;
  int cc;
  uint32_t csrc[15];
};

// RTP sender/receiver state
struct rtp_state {
  uint32_t ssrc;
  int init;
  uint16_t seq;
  uint32_t timestamp;
  long long packets;
  long long bytes;
  long long drops;
  long long dupes;
};

// Internal format of sender report segment
struct rtcp_sr {
  unsigned int ssrc;
  long long ntp_timestamp;
  unsigned int rtp_timestamp;
  unsigned int packet_count;
  unsigned int byte_count;
};

// Internal format of receiver report segment
struct rtcp_rr {
  unsigned int ssrc;
  int lost_fract;
  int lost_packets;
  int highest_seq;
  int jitter;
  int lsr; // Last SR
  int dlsr; // Delay since last SR
};

// Internal format of RTCP source description
enum sdes_type {
  CNAME=1,
  NAME=2,
  EMAIL=3,
  PHONE=4,
  LOC=5,
  TOOL=6,
  NOTE=7,
  PRIV=8,
};

// Individual source description item
struct rtcp_sdes {
  enum sdes_type type;
  uint32_t ssrc;
  int mlen;
  char message[256];
};

// Convert between internal and wire representations of RTP header
unsigned char *ntoh_rtp(struct rtp_header *,unsigned char *);
unsigned char *hton_rtp(unsigned char *, struct rtp_header *);

int setup_mcast(char const *target,int output,int ttl,int offset);
extern char Default_mcast_port[];

// Function to process incoming RTP packet headers
// Returns number of samples dropped or skipped by silence suppression, if any
int rtp_process(struct rtp_state *state,struct rtp_header *rtp,int samples);

// Generate RTCP source description segment
unsigned char *gen_sdes(unsigned char *output,int bufsize,uint32_t ssrc,struct rtcp_sdes const *sdes,int sc);
// Generate RTCP bye segment
unsigned char *gen_bye(unsigned char *output,int bufsize,uint32_t const *ssrcs,int sc);
// Generate RTCP sender report segment
unsigned char *gen_sr(unsigned char *output,int bufsize,struct rtcp_sr const *sr,struct rtcp_rr const *rr,int rc);
// Generate RTCP receiver report segment
unsigned char *gen_rr(unsigned char *output,int bufsize,uint32_t ssrc,struct rtcp_rr const *rr,int rc);

// Utility routines for reading from, and writing integers to, network format in char buffers
static inline unsigned short get8(unsigned char const *dp){
  assert(dp != NULL);
  return *dp;
}

static inline unsigned short get16(unsigned char const *dp){
  assert(dp != NULL);
  return dp[0] << 8 | dp[1];
}

static inline unsigned long get24(unsigned char const *dp){
  assert(dp != NULL);
  return dp[0] << 16 | dp[1] << 8 | dp[2];
}

static inline unsigned long get32(unsigned char const *dp){
  assert(dp != NULL);
  return dp[0] << 24 | dp[1] << 16 | dp[2] << 8 | dp[3];
}

static inline unsigned char *put8(unsigned char *dp,uint8_t x){
  assert(dp != NULL);
  *dp++ = x;
  return dp;
}

static inline unsigned char *put16(unsigned char *dp,uint16_t x){
  assert(dp != NULL);
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}

static inline unsigned char *put24(unsigned char *dp,uint32_t x){
  assert(dp != NULL);
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}

static inline unsigned char *put32(unsigned char *dp,uint32_t x){
  assert(dp != NULL);
  *dp++ = x >> 24;
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}


#endif
