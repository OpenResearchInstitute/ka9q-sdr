// $Id: multicast.c,v 1.22 2018/07/06 06:06:12 karn Exp $
// Multicast socket and RTP utility routines
// Copyright 2018 Phil Karn, KA9Q

#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include "multicast.h"

int Mcast_ttl = 1;


// Set options on multicast socket
static void soptions(int fd){
  // Failures here are not fatal
  int reuse = 1;
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseport failed");
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseaddr failed");
  struct linger linger;
  linger.l_onoff = 0;
  linger.l_linger = 0;
  if(setsockopt(fd,SOL_SOCKET,SO_LINGER,&linger,sizeof(linger)) != 0)
    perror("so_linger failed");
  u_char ttl = Mcast_ttl;
  if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl)) != 0){
    perror("so_ttl failed");
  }
  u_char loop = 1;
  if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop)) != 0){
    perror("so_ttl failed");
  }
}

// Join a socket to a multicast group
#if defined(linux) // Linux, etc, for both IPv4/IPv6
static int join_group(int fd,struct addrinfo *resp){
  struct sockaddr_in const *sin = (struct sockaddr_in *)resp->ai_addr;
  if(!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
    return -1;

  struct group_req group_req;
  group_req.gr_interface = 0;
  memcpy(&group_req.gr_group,resp->ai_addr,resp->ai_addrlen);
  if(setsockopt(fd,IPPROTO_IP,MCAST_JOIN_GROUP,&group_req,sizeof(group_req)) != 0){
    perror("multicast join");
    return -1;
  }
  return 0;
}
#else // old version, seems required on Apple    
static int join_group(int fd,struct addrinfo *resp){
  struct sockaddr_in const *sin = (struct sockaddr_in *)resp->ai_addr;
  if(!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
     return -1;

  struct ip_mreq mreq;
  mreq.imr_multiaddr = sin->sin_addr;
  mreq.imr_interface.s_addr = INADDR_ANY;
  if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0){
    perror("multicast join");
    return -1;
  }
  return 0;
}
#endif

char Default_mcast_port[] = "5004";

// Set up multicast socket for input or output
// Target is in the form of domain.name.com:5004 or 1.2.3.4:5004
// when output = 1, connect to the multicast address so we can simply send() to it without specifying a destination
// when output = 0, bind to it so we'll accept incoming packets
// (Can we just do both?)
int setup_mcast(char const *target,int output){
  int len = strlen(target) + 1;  // Including terminal null
  char host[len],*port;

  strlcpy(host,target,len);
  if((port = strrchr(host,':')) != NULL){
    *port++ = '\0';
  } else {
    port = Default_mcast_port; // Default for RTP
  }

  struct addrinfo hints;
  memset(&hints,0,sizeof(hints));
  hints.ai_family = AF_INET; // Only IPv4 for now (grrr....)
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICSERV;

  struct addrinfo *results = NULL;
  int ecode;
  if((ecode = getaddrinfo(host,port,&hints,&results)) != 0){
    fprintf(stderr,"setup_mcast getaddrinfo(%s,%s): %s\n",host,port,gai_strerror(ecode));
    return -1;
  }
  struct addrinfo *resp;
  int fd = -1;
  for(resp = results; resp != NULL; resp = resp->ai_next){
    if((fd = socket(resp->ai_family,resp->ai_socktype,resp->ai_protocol)) < 0)
      continue;
    soptions(fd);
    if(output){
      if((connect(fd,resp->ai_addr,resp->ai_addrlen) == 0))
	break;
    } else { // input
      if((bind(fd,resp->ai_addr,resp->ai_addrlen) == 0))
	break;
    }
    close(fd);
    fd = -1;
  }
  // Strictly speaking, it is not necessary to join a multicast group to which we only send.
  // But this creates a problem with brain-dead Netgear (and probably other) "smart" switches
  // that do IGMP snooping. There's a setting to handle what happens with multicast groups
  // to which no IGMP messages are seen. If set to discard them, IPv6 multicast breaks
  // because there's no IPv6 multicast querier. But set to pass them, then IPv4 multicasts
  // that aren't subscribed to by anybody are flooded everywhere! We avoid that by subscribing
  // to our own multicasts.

  if(fd != -1)
    join_group(fd,resp);
  else
    fprintf(stderr,"setup_input: Can't create multicast socket for %s:%s\n",host,port);

#if 0 // testing hack - find out if we're using source specific multicast (we're not)
  {
  uint32_t fmode  = MCAST_INCLUDE;
  uint32_t numsrc = 100;
  struct sockaddr_storage slist[100];

  int n;
  n = getsourcefilter(fd,0,resp->ai_addr,resp->ai_addrlen,&fmode,&numsrc,slist);
  if(n < 0)
    perror("getsourcefilter");
  printf("n = %d numsrc = %d\n",n,numsrc);
  }
#endif

  freeaddrinfo(results);
  return fd;
}

// Convert RTP header from network (wire) big-endian format to internal host structure
// Written to be insensitive to host byte order and C structure layout and padding
// Use of unsigned formats is important to avoid unwanted sign extension

static inline unsigned short get16(unsigned char *dp){
  return dp[0] << 8 | dp[1];
}
static inline unsigned long get32(unsigned char *dp){
  return dp[0] << 24 | dp[1] << 16 | dp[2] << 8 | dp[3];
}

unsigned char *ntoh_rtp(struct rtp_header *rtp,unsigned char *data){
  unsigned char *dp = data;

  rtp->version = *dp >> 6; // What should we do if it's not 2??
  rtp->pad = (*dp >> 5) & 1;
  rtp->extension = (*dp >> 4) & 1;
  rtp->cc = *dp & 0xf;
  dp++;

  rtp->marker = (*dp >> 7) & 1;
  rtp->type = *dp & 0x7f;
  dp++;

  rtp->seq = get16(dp);
  dp += 2;

  rtp->timestamp = get32(dp);
  dp += 4;
  
  rtp->ssrc = get32(dp);
  dp += 4;

  for(int i=0; i<rtp->cc; i++){
    rtp->csrc[i] = get32(dp);
    dp += 4;
  }

  if(rtp->extension){
    // Ignore any extension, but skip over it
    dp += 2; // skip over type
    uint16_t ext_len = 4 + get16(dp); // grab length
    dp += 2;
    dp += ext_len;
  }
  return dp;
}


// Convert RTP header from internal host structure to network (wire) big-endian format
// Written to be insensitive to host byte order and C structure layout and padding

static inline unsigned char *put16(unsigned char *dp,uint16_t x){
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}
static inline unsigned char *put32(unsigned char *dp,uint32_t x){
  *dp++ = x >> 24;
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  return dp;
}


unsigned char *hton_rtp(unsigned char *data, struct rtp_header *rtp){
  rtp->cc &= 0xf; // Force it to be legal
  rtp->type &= 0x7f;
  *data++ = (RTP_VERS << 6) | (rtp->pad << 5) | (rtp->extension << 4) | rtp->cc; // Force version 2
  *data++ = (rtp->marker << 7) | rtp->type;
  data = put16(data,rtp->seq);
  data = put32(data,rtp->timestamp);
  data = put32(data,rtp->ssrc);
  for(int i=0; i < rtp->cc; i++)
    data = put32(data,rtp->csrc[i]);
  
  return data;
}


// Process sequence number and timestamp in incoming RTP header:
// Check that the sequence number is (close to) what we expect
// If not, drop it but 3 wild sequence numbers in a row will assume a stream restart
//
// Determine timestamp jump, if any
// Returns: <0            if packet should be dropped as a duplicate or a wild sequence number
//           0            if packet is in sequence with no missing timestamps
//         timestamp jump if packet is in sequence or <10 sequence numbers ahead, with missing timestamps
int rtp_process(struct rtp_state *state,struct rtp_header *rtp,int sampcnt){
  state->ssrc = rtp->ssrc; // Must be filtered elsewhere if you want it
  state->packets++;
  if(!state->init){
    state->expected_seq = rtp->seq;
    state->expected_timestamp = rtp->timestamp;
    state->init = 1;
  }
  // Sequence number check
  short seq_step = (short)(rtp->seq - state->expected_seq);
  if(seq_step != 0){
    if(seq_step > 0)
      state->drops++;	  
    else if(seq_step < 0)
      state->dupes++;

    if(++state->seq_err < 3){ // Look for three consecutive out-of-sequence packets
      return -1;
    }
    // Three invalid sequence numbers in a row; probably a restarted stream so accept this sequence number
    state->drops = state->dupes = 0; // Not really meaningful after a resync
    state->expected_timestamp = rtp->timestamp;
    state->resyncs++;
  }
  state->seq_err = 0;
  state->expected_seq = rtp->seq + 1;

  int time_step = (int)(rtp->timestamp - state->expected_timestamp);
  if(time_step < 0)
    return time_step;    // Old samples; drop. Shouldn't happen if sequence number isn't old

  state->expected_timestamp = rtp->timestamp + sampcnt;
  return time_step;
}

