#ifndef _MULTICAST_H
#define _MULTICAST_H
int setup_mcast(char const *target,int output);
extern char Default_mcast_port[];
extern int Mcast_ttl;

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
#define RTP_MIN_SIZE 12  // min size of RTP header
#define RTP_VERS 2
#define RTP_MARKER 0x80  // Marker flag in mpt field

#define IQ_PT (97) // NON-standard payload type for my raw I/Q streams
#define PCM_MONO_PT (11)
#define PCM_STEREO_PT (10)

#define OPUS_PT (111) // Hard-coded NON-standard payload type for OPUS (should be dynamic with sdp)

unsigned char *ntoh_rtp(struct rtp_header *,unsigned char *);
unsigned char *hton_rtp(unsigned char *, struct rtp_header *);

struct rtp_state {
  int init;
  uint16_t expected_seq;
  uint32_t expected_timestamp;
  long long drops;
  long long dupes;
  int reseq;
};

int rtp_process(struct rtp_state *state,struct rtp_header *rtp,int samples);


#endif
