#ifndef _MULTICAST_H
#define _MULTICAST_H
int setup_mcast(char const *target,int output);
extern char Default_mcast_port[];
extern int Mcast_ttl;

struct rtp_header {
  uint8_t vpxcc;
  uint8_t mpt;
  uint16_t seq;
  uint32_t timestamp;
  uint32_t ssrc;
};
#define RTP_VERS 2
#define RTP_MARKER 0x80  // Marker flag in mpt field

#define PCM_MONO_PT (11)
#define PCM_STEREO_PT (10)
// Hard-coded RTP payload type for OPUS (NOT STANDARD! should be dynamic with sdp)
#define OPUS_PT (111)


#endif
