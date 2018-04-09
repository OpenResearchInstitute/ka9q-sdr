// $Id: opus.c,v 1.14 2018/04/04 05:55:07 karn Exp $
// Opus compression relay
// Read PCM audio from one multicast group, compress with Opus and retransmit on another
// Currently subject to memory leaks as old group states aren't yet aged out
// Copyright Jan 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <string.h>
#include <opus/opus.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netdb.h>
#include <locale.h>
#include <errno.h>

#include "misc.h"
#include "multicast.h"

// Global config variables
char *Mcast_input_address_text = "audio-pcm-mcast.local";     // Multicast address we're listening to
char *Mcast_output_address_text = "audio-opus-mcast.local";     // Multicast address we're sending to

int const Bufsize = 8192;     // Maximum samples/words per RTP packet - must be bigger than Ethernet MTU
int const Samprate = 48000;   // Too hard to handle other sample rates right now
                              // Opus will notice the actual audio bandwidth, so there's no real cost to this
int Verbose;                  // Verbosity flag (currently unused)

int Input_fd = -1;            // Multicast receive socket
int Output_fd = -1;           // Multicast receive socket
float Opus_blocktime = 20;    // 20 ms, a reasonable default
int Opus_frame_size;
int Opus_bitrate = 32;        // Opus stream audio bandwidth; default 32 kb/s
int const Channels = 2;       // Stereo - no penalty if the audio is actually mono, Opus will figure it out
int Discontinuous = 0;        // Off by default
int Fec = 0;                  // Use forward error correction


float const SCALE = 1./SHRT_MAX;


struct session {
  struct session *prev;       // Linked list pointers
  struct session *next; 
  uint32_t ssrc;            // RTP Sending Source ID
  int eseq;                 // Next expected RTP input sequence number
  int etimestamp;           // Next expected RTP input timestamp
  int type;                 // input RTP type (10,11)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port
  OpusEncoder *opus;        // Opus encoder handle
  int silence;              // Currently suppressing silence

  int oseq;                 // Output sequence number
  int otimestamp;           // Output timestamp
  float *audio_buffer;      // Buffer to accumulate PCM until enough for Opus frame
  int audio_index;          // Index of next sample to write into audio_buffer

  unsigned long packets;    // RTP packets for this session
  unsigned long drops;      // Apparent rtp packet drops
  unsigned long invalids;   // Unknown RTP type
  unsigned long empties;    // RTP but no data
  unsigned long dupes;      // Duplicate or old serial numbers
  unsigned long underruns;  // Callback count of underruns (stereo samples) replaced with silence
};
struct session *Audio;



void closedown(int);
struct session *lookup_session(const struct sockaddr *,uint32_t);
struct session *make_session(struct sockaddr const *r,uint32_t,uint16_t,uint32_t);
int close_session(struct session *);
int send_samples(struct session *sp,float left,float right);


int main(int argc,char * const argv[]){
  // Try to improve our priority
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Drop root if we have it
  seteuid(getuid());

  setlocale(LC_ALL,getenv("LANG"));

  int c;
  Mcast_ttl = 5; // By default, let Opus be routed
  while((c = getopt(argc,argv,"f:I:vR:B:o:xT:")) != EOF){
    switch(c){
    case 'f':
      Fec = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      Mcast_input_address_text = optarg;
      break;
    case 'R':
      Mcast_output_address_text = optarg;
      break;
    case 'B':
      Opus_blocktime = strtod(optarg,NULL);
      break;
    case 'o':
      Opus_bitrate = strtol(optarg,NULL,0);
      break;
    case 'x':
      Discontinuous = 1;
      break;
    default:
      fprintf(stderr,"Usage: %s [-x] [-v] [-o bitrate] [-B blocktime] [-I input_mcast_address] [-R output_mcast_address][-T mcast_ttl]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -o %d -B %.1f -I %s -R %s -T %d\n",argv[0],Opus_bitrate,Opus_blocktime,Mcast_input_address_text,Mcast_output_address_text,Mcast_ttl);
      exit(1);
    }
  }
  if(Opus_blocktime != 2.5 && Opus_blocktime != 5
     && Opus_blocktime != 10 && Opus_blocktime != 20
     && Opus_blocktime != 40 && Opus_blocktime != 60
     && Opus_blocktime != 80 && Opus_blocktime != 100
     && Opus_blocktime != 120){
    fprintf(stderr,"opus block time must be 2.5/5/10/20/40/60/80/100/120 ms\n");
    fprintf(stderr,"80/100/120 supported only on opus 1.2 and later\n");
    exit(1);
  }
  Opus_frame_size = round(Opus_blocktime * Samprate / 1000.);
  if(Opus_bitrate < 500)
    Opus_bitrate *= 1000; // Assume it was given in kb/s

  // Set up multicast
  Input_fd = setup_mcast(Mcast_input_address_text,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up input on %s: %sn",Mcast_input_address_text,strerror(errno));
    exit(1);
  }
  Output_fd = setup_mcast(Mcast_output_address_text,1);
  if(Output_fd == -1){
    fprintf(stderr,"Can't set up output on %s: %s\n",Mcast_output_address_text,strerror(errno));
    exit(1);
  }

  // Set up to receive PCM in RTP/UDP/IP
  struct iovec iovec_in[2];
  struct rtp_header rtp_in;
  signed short data_in[Bufsize];
  
  iovec_in[0].iov_base = &rtp_in;
  iovec_in[0].iov_len = sizeof(rtp_in);
  iovec_in[1].iov_base = data_in;
  iovec_in[1].iov_len = sizeof(data_in);

  struct msghdr message_in;
  struct sockaddr sender;
  message_in.msg_name = &sender;
  message_in.msg_namelen = sizeof(sender);
  message_in.msg_iov = &iovec_in[0];
  message_in.msg_iovlen = 2;
  message_in.msg_control = NULL;
  message_in.msg_controllen = 0;
  message_in.msg_flags = 0;

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  while(1){
    int size;

    size = recvmsg(Input_fd,&message_in,0);
    if(size == -1){
      if(errno != EINTR){ // Happens routinely
	perror("recvmsg");
	usleep(1000);
      }
      continue;
    }
    if(size < sizeof(rtp_in)){
      usleep(500); // Avoid tight loop
      continue; // Too small to be valid RTP
    }
    // To host order
    rtp_in.ssrc = ntohl(rtp_in.ssrc);
    rtp_in.seq = ntohs(rtp_in.seq);
    rtp_in.timestamp = ntohl(rtp_in.timestamp);
    int marker = rtp_in.mpt & RTP_MARKER;
    rtp_in.mpt &= ~RTP_MARKER;

    // Only accept mono and stereo PCM at implied 48 kHz sample rate
    if(rtp_in.mpt != PCM_STEREO_PT && rtp_in.mpt != PCM_MONO_PT) // 1 byte, no need to byte swap
      goto endloop; // Discard all but mono and stereo PCM to avoid polluting session table

    struct session *sp = lookup_session(&sender,rtp_in.ssrc);
    if(sp == NULL){
      // Not found
      if((sp = make_session(&sender,rtp_in.ssrc,rtp_in.seq,rtp_in.timestamp)) == NULL){
	fprintf(stderr,"No room!!\n");
	goto endloop;
      }
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);
      sp->oseq = 0;
      sp->dupes = 0;
      sp->audio_buffer = malloc(Channels * sizeof(float) * Opus_frame_size);
      sp->audio_index = 0;
      int error = 0;
      sp->opus = opus_encoder_create(Samprate,Channels,OPUS_APPLICATION_AUDIO,&error);
      if(error != OPUS_OK || !sp->opus){
	fprintf(stderr,"opus_encoder_create error %d\n",error);
	exit(1);
      }
      error = opus_encoder_ctl(sp->opus,OPUS_SET_DTX(Discontinuous));
      if(error != OPUS_OK)
	fprintf(stderr,"opus_encoder_ctl set discontinuous %d: error %d\n",Discontinuous,error);

      error = opus_encoder_ctl(sp->opus,OPUS_SET_BITRATE(Opus_bitrate));
      if(error != OPUS_OK)
	fprintf(stderr,"opus_encoder_ctl set bitrate %d: error %d\n",Opus_bitrate,error);

      if(Fec){
	error = opus_encoder_ctl(sp->opus,OPUS_SET_INBAND_FEC(1));
	if(error != OPUS_OK)
	  fprintf(stderr,"opus_encoder_ctl set FEC on error %d\n",error);
	error = opus_encoder_ctl(sp->opus,OPUS_SET_PACKET_LOSS_PERC(Fec));
	if(error != OPUS_OK)
	  fprintf(stderr,"opus_encoder_ctl set FEC loss rate %d%% error %d\n",Fec,error);
      }

      // Always seems to return error -5 even when OK??
      error = opus_encoder_ctl(sp->opus,OPUS_FRAMESIZE_ARG,Opus_blocktime);
      if(0 && error != OPUS_OK)
	fprintf(stderr,"opus_encoder_ctl set framesize %d (%.1lf ms): error %d\n",Opus_frame_size,Opus_blocktime,error);
    }
    sp->packets++;
    sp->type = rtp_in.mpt;
    if(rtp_in.seq != sp->eseq){
      int const diff = (int)(rtp_in.seq - sp->eseq);
      //        fprintf(stderr,"ssrc %lx: expected %d got %d\n",(unsigned long)rtp.ssrc,sp->eseq,rtp.seq);
      if(diff < 0 && diff > -10){
	sp->dupes++;
	goto endloop;	// Drop probable duplicate
      }
      sp->drops += abs(diff); // Apparent # packets dropped
    }
    sp->eseq = (rtp_in.seq + 1) & 0xffff;

    size -= sizeof(rtp_in); // Bytes in payload
    if(size <= 0){
      sp->empties++;
      goto endloop; // empty?!
    }

    int samples_skipped = (signed int)(rtp_in.timestamp - sp->etimestamp);

    sp->otimestamp += samples_skipped; // Track sender's timestamp jumps
    if(marker || samples_skipped > 4*Opus_frame_size){
      // reset encoder state after 4 frames of complete silence and set RTP marker bit
      opus_encoder_ctl(sp->opus,OPUS_RESET_STATE);
      sp->silence = 1;
    }
    int samples = 0;
    switch(rtp_in.mpt){
    case PCM_STEREO_PT: // Stereo
      samples = size / 4;  // # 32-bit word samples
      for(int i=0; i < samples; i++){
	float left = SCALE * (signed short)ntohs(data_in[2*i]);
	float right = SCALE * (signed short)ntohs(data_in[2*i+1]);
	send_samples(sp,left,right);
      }
      break;
    case PCM_MONO_PT: // Mono; send to both stereo channels
      samples = size / 2;
      for(int i=0;i<samples;i++){
	float left = SCALE * (signed short)ntohs(data_in[i]);	
	float right = left;
	send_samples(sp,left,right);
      }
      break;
    default:
      samples = 0;
      break; // ignore
    }
    sp->etimestamp = rtp_in.timestamp + samples;

  endloop:;
  }
  exit(0);
}

struct session *lookup_session(const struct sockaddr *sender,const uint32_t ssrc){
  struct session *sp;
  for(sp = Audio; sp != NULL; sp = sp->next){
    if(sp->ssrc == ssrc && memcmp(&sp->sender,sender,sizeof(*sender)) == 0){
      // Found it
      if(sp->prev != NULL){
	// Not at top of bucket chain; move it there
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;

	sp->prev->next = sp->next;
	sp->prev = NULL;
	sp->next = Audio;
	Audio = sp;
      }
      return sp;
    }
  }
  return NULL;
}
// Create a new session, partly initialize
struct session *make_session(struct sockaddr const *sender,uint32_t ssrc,uint16_t seq,uint32_t timestamp){
  struct session *sp;

  if((sp = calloc(1,sizeof(*sp))) == NULL)
    return NULL; // Shouldn't happen on modern machines!
  
  // Initialize entry
  memcpy(&sp->sender,sender,sizeof(struct sockaddr));
  sp->ssrc = ssrc;
  sp->eseq = seq;
  sp->etimestamp = timestamp;

  // Put at head of bucket chain
  sp->next = Audio;
  if(sp->next != NULL)
    sp->next->prev = sp;
  Audio = sp;
  return sp;
}

int close_session(struct session *sp){
  if(sp == NULL)
    return -1;
  
  if(sp->opus != NULL){
    opus_encoder_destroy(sp->opus);
    sp->opus = NULL;
  }
  if(sp->audio_buffer)
    free(sp->audio_buffer);
  sp->audio_buffer = NULL;

  // Remove from linked list
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Audio = sp->next;
  free(sp);
  return 0;
}
void closedown(int s){
  while(Audio != NULL)
    close_session(Audio);

  exit(0);
}
// Enqueue a stereo pair of samples for transmit, encode and send Opus
// frame when we have enough
int send_samples(struct session *sp,float left,float right){
  int size = 0;
  sp->audio_buffer[sp->audio_index++] = left;
  sp->audio_buffer[sp->audio_index++] = right;  
  if(sp->audio_index >= Opus_frame_size * Channels){
    unsigned char data_out[Bufsize];
    sp->audio_index = 0;
    
    size += opus_encode_float(sp->opus,sp->audio_buffer,Opus_frame_size,data_out,sizeof(data_out));
    if(!Discontinuous || size > 2){
      // ship it
      // Set up to transmit Opus RTP/UDP/IP
      struct iovec iovec_out[2];
      struct rtp_header rtp_out;
      
      rtp_out.vpxcc = RTP_VERS << 6;
      
      iovec_out[0].iov_base = &rtp_out;
      iovec_out[0].iov_len = sizeof(rtp_out);
      iovec_out[1].iov_base = data_out;
      iovec_out[1].iov_len = size;
      
      struct msghdr message_out;
      message_out.msg_name = NULL; // connected-mode socket already has destination
      message_out.msg_namelen = 0;
      message_out.msg_iov = &iovec_out[0];
      message_out.msg_iovlen = 2;
      message_out.msg_control = NULL;
      message_out.msg_controllen = 0;
      message_out.msg_flags = 0;

      rtp_out.seq = htons(sp->oseq++);
      rtp_out.mpt = OPUS_PT; // Opus
      if(sp->silence){
	// Beginning of talk spurt after silence, set marker bit
	rtp_out.mpt |= RTP_MARKER;
	sp->silence = 0;
      }
      rtp_out.ssrc = htonl(sp->ssrc);
      rtp_out.timestamp = htonl(sp->otimestamp);
      size = sendmsg(Output_fd,&message_out,0);
    } else
      sp->silence = 1;
    sp->otimestamp += Opus_frame_size;
  }
  return size;
}
