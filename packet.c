// $Id: packet.c,v 1.23 2018/09/08 06:06:21 karn Exp $
// AFSK/FM packet demodulator
// Reads RTP PCM audio stream, emits decoded frames in multicast RTP
// Copyright 2018, Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <netdb.h>

#include "dsp.h"
#include "filter.h"
#include "misc.h"
#include "multicast.h"
#include "ax25.h"



// Needs to be redone with common RTP receiver module
struct session {
  struct session *next; 
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port

  struct rtp_state rtp_state_in;
  struct rtp_state rtp_state_out;

  int input_pointer;
  struct filter_in *filter_in;
  pthread_t decode_thread;
  unsigned int decoded_packets;
};

// Config constants
#define MAX_MCAST 20          // Maximum number of multicast addresses
float const SCALE = 1./32768;
int const Bufsize = 2048;
int const AN = 2048; // Should be power of 2 for FFT efficiency
int const AL = 1000; // 25 bit times
//int const AM = AN - AL + 1; // should be >= Samppbit, i.e., samprate / bitrate
int const AM = 1049;
float const Samprate = 48000;
float const Bitrate = 1200;
//int const Samppbit = Samprate/Bitrate;
int const Samppbit = 40;

// Command line params
char *Mcast_address_text[MAX_MCAST];
char *Decode_mcast_address_text = "ax25.mcast.local";
int Verbose;
int Mcast_ttl = 10;           // Very low intensity output

// Global variables
int Nfds;                     // Number of streams
int Input_fd = -1;
int Output_fd = -1;
struct session *Session;
extern float Kaiser_beta;
pthread_mutex_t Output_mutex;

struct session *lookup_session(const struct sockaddr *sender,const uint32_t ssrc);
struct session *make_session(struct sockaddr const *sender,uint32_t ssrc);
int close_session(struct session *sp);

void *decode_task(void *arg);

int main(int argc,char *argv[]){
  // Drop root if we have it
  if(seteuid(getuid()) != 0)
    fprintf(stderr,"seteuid: %s\n",strerror(errno));

  setlocale(LC_ALL,getenv("LANG"));
  // Unlike aprs and aprsfeed, stdout is not line buffered because each packet
  // generates a multi-line dump. So we have to be sure to fflush(stdout) after each
  // packet in case we're redirected into a file

  int c;
  while((c = getopt(argc,argv,"I:R:vT:")) != EOF){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'I':
      if(Nfds == MAX_MCAST){
	fprintf(stderr,"Too many multicast addresses; max %d\n",MAX_MCAST);
      } else 
	Mcast_address_text[Nfds++] = optarg;
      break;
    case 'R':
      Decode_mcast_address_text = optarg;
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    default:
      fprintf(stderr,"Usage: %s [-v] [-I input_mcast_address] [-R output_mcast_address] [-T mcast_ttl]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -I [none] -R %s -T %d\n",argv[0],Decode_mcast_address_text,Mcast_ttl);
      exit(1);
    }
  }
  if(Nfds == 0){
    fprintf(stderr,"At least one -I option required\n");
    exit(1);
  }
  // Set up multicast input, create mask for select()
  fd_set fdset_template; // Mask for select()
  FD_ZERO(&fdset_template);
  int max_fd = 2;        // Highest number fd for select()
  int input_fd[Nfds];    // Multicast receive sockets

  for(int i=0;i<Nfds;i++){
    input_fd[i] = setup_mcast(Mcast_address_text[i],0,0,0);
    if(input_fd[i] == -1){
      fprintf(stderr,"Can't set up input %s\n",Mcast_address_text[i]);
      continue;
    }
    if(input_fd[i] > max_fd)
      max_fd = input_fd[i];
    FD_SET(input_fd[i],&fdset_template);
  }
  Output_fd = setup_mcast(Decode_mcast_address_text,1,Mcast_ttl,0);
  if(Output_fd == -10){
    fprintf(stderr,"Can't set up output to %s\n",
	    Decode_mcast_address_text);
    exit(1);
  }
  pthread_mutex_init(&Output_mutex,NULL);

  struct rtp_header rtp_hdr;
  struct sockaddr sender;
  // audio input thread
  // Receive audio multicasts, multiplex into sessions, execute filter front end (which wakes up decoder thread)
  while(1){
    // Wait for traffic to arrive
    fd_set fdset = fdset_template;
    int s = select(max_fd+1,&fdset,NULL,NULL,NULL);
    if(s < 0 && errno != EAGAIN && errno != EINTR)
      break;
    if(s == 0)
      continue; // Nothing arrived; probably just an ignored signal

    for(int fd_index = 0;fd_index < Nfds;fd_index++){
      if(input_fd[fd_index] == -1 || !FD_ISSET(input_fd[fd_index],&fdset))
	continue;

      unsigned char buffer[16384]; // Fix this
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(input_fd[fd_index],buffer,sizeof(buffer),0,&sender,&socksize);
      if(size == -1){
	if(errno != EINTR){ // Happens routinely
	  perror("recvfrom");
	  usleep(1000);
	}
	continue;
      }
      if(size < RTP_MIN_SIZE){
	usleep(1000); // Avoid tight loop
	continue; // Too small to be valid RTP
      }
      // Extract RTP header
      unsigned char *dp = buffer;
      dp = ntoh_rtp(&rtp_hdr,dp);
      size -= dp - buffer;
      
      if(rtp_hdr.pad){
	// Remove padding
	size -= dp[size-1];
	rtp_hdr.pad = 0;
      }
      
      if(rtp_hdr.type != PCM_MONO_PT)
	continue; // Only mono PCM for now
      
      struct session *sp = lookup_session(&sender,rtp_hdr.ssrc);
      if(sp == NULL){
	// Not found
	if((sp = make_session(&sender,rtp_hdr.ssrc)) == NULL){
	  fprintf(stdout,"No room for new session!!\n");
	  fflush(stdout);
	  continue;
	}
	sp->rtp_state_out.ssrc = sp->rtp_state_in.ssrc = rtp_hdr.ssrc;
	getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);
	sp->input_pointer = 0;
	sp->filter_in = create_filter_input(AL,AM,REAL);
	pthread_create(&sp->decode_thread,NULL,decode_task,sp); // One decode thread per stream
	if(Verbose){
	  fprintf(stdout,"New session from %s, ssrc %x\n",sp->addr,sp->rtp_state_in.ssrc);
	  fflush(stdout);
	}
      }
      int sample_count = size / sizeof(signed short); // 16-bit sample count
      int skipped_samples = rtp_process(&sp->rtp_state_in,&rtp_hdr,sample_count);
      if(skipped_samples < 0)
	continue;	// Drop probable duplicate(s)
      
      // Ignore skipped_samples > 0; no real need to maintain sample count when squelch closes
      // Even if its caused by dropped RTP packets there's no FEC to fix it anyway
      signed short *samples = (signed short *)dp;
      while(sample_count-- > 0){
	// Swap sample to host order, convert to float
	sp->filter_in->input.r[sp->input_pointer++] = ntohs(*samples++) * SCALE;
	if(sp->input_pointer == sp->filter_in->ilen){
	  execute_filter_input(sp->filter_in); // Wakes up any threads waiting for data on this filter
	  sp->input_pointer = 0;
	}
      }
    }
  }
  // Need to kill decoder threads? Or will ordinary signals reach them?
  exit(0);
}



// Find existing session in table, if it exists
struct session *lookup_session(const struct sockaddr *sender,const uint32_t ssrc){
  struct session *sp;
  for(sp = Session; sp != NULL; sp = sp->next){
    if(sp->rtp_state_in.ssrc == ssrc && memcmp(&sp->sender,sender,sizeof(*sender)) == 0){
      // Found it
      return sp;
    }
  }
  return NULL;
}
// Create a new session, partly initialize
struct session *make_session(struct sockaddr const *sender,uint32_t ssrc){
  struct session *sp;

  if((sp = calloc(1,sizeof(*sp))) == NULL)
    return NULL; // Shouldn't happen on modern machines!
  
  // Initialize entry
  memcpy(&sp->sender,sender,sizeof(struct sockaddr));
  sp->rtp_state_in.ssrc = ssrc;

  // Put at head of bucket chain
  sp->next = Session;
  Session = sp;
  return sp;
}

int close_session(struct session *sp){
  if(sp == NULL)
    return -1;
  
  // Remove from linked list
  struct session *se,*se_prev = NULL;
  for(se = Session; se && se != sp; se_prev = se,se = se->next)
    ;
  if(!se)
    return -1;
  
  if(se == sp){
    if(se_prev)
      se_prev->next = sp->next;
    else
      Session = se_prev;
  }
  return 0;
}

// AFSK demod, HDLC decode
void *decode_task(void *arg){
  pthread_setname("afsk");
  struct session *sp = (struct session *)arg;
  assert(sp != NULL);

  struct filter_out *filter = create_filter_output(sp->filter_in,NULL,1,COMPLEX);
  set_filter(filter,Samprate,+100,+4000,3.0); // Creates analytic, band-limited signal

  // Tone replica generators (-1200 and -2200 Hz)
  float complex mark_phase = 1;
  float complex mark_step = csincosf(-2*M_PI*1200./Samprate);
  float complex space_phase = 1;
  float complex space_step = csincosf(-2*M_PI*2200./Samprate);

  // Tone integrators
  int symphase = 0;
  float complex mark_accum = 0; // On-time
  float complex space_accum = 0;
  float complex mark_offset_accum = 0; // Straddles previous zero crossing
  float complex space_offset_accum = 0;
  float last_val = 0;  // Last on-time symbol
  float mid_val = 0;   // Last zero crossing symbol

  // hdlc state
  unsigned char hdlc_frame[1024];
  memset(hdlc_frame,0,sizeof(hdlc_frame));
  int frame_bit = 0;
  int flagsync = 0;
  int ones = 0;

  while(1){
    execute_filter_output(filter);    // Blocks until data appears

    for(int n=0; n<filter->olen; n++){

      // Spin down by 1200 and 2200 Hz, accumulate each in boxcar (comb) filters
      // Mark and space each have in-phase and offset integrators for timing recovery
      float complex s;
      s = mark_phase * filter->output.c[n];
      mark_phase *= mark_step;
      mark_accum += s;
      mark_offset_accum += s;

      s = space_phase * filter->output.c[n];
      space_phase *= space_step;
      space_accum += s;
      space_offset_accum += s;

      if(++symphase == Samppbit/2){
	// Finish offset integrator and reset
	mid_val = cnrmf(mark_offset_accum) - cnrmf(space_offset_accum);
	mark_offset_accum = space_offset_accum = 0;
      }
      if(symphase < Samppbit)
	continue;
      
      // Finished whole bit
      symphase = 0;
      float cur_val = cnrmf(mark_accum) - cnrmf(space_accum);
      mark_accum = space_accum = 0;

      assert(frame_bit >= 0);
      if(cur_val * last_val < 0){
	// Transition -- Gardner-style clock adjust
	symphase += ((cur_val - last_val) * mid_val) > 0 ? +1 : -1;

	// NRZI zero
	if(ones == 6){
	  // Flag
	  if(flagsync){
	    frame_bit -= 7; // Remove 0111111
	    int bytes = frame_bit / 8;
	    if(bytes > 0 && crc_good(hdlc_frame,bytes)){
	      if(Verbose){
		time_t t;
		struct tm *tmp;
		time(&t);
		tmp = gmtime(&t);
		// Lock output to prevent intermingled output
		pthread_mutex_lock(&Output_mutex);

		fprintf(stdout,"%d %s %04d %02d:%02d:%02d UTC ",tmp->tm_mday,Months[tmp->tm_mon],tmp->tm_year+1900,
		       tmp->tm_hour,tmp->tm_min,tmp->tm_sec);
		
		fprintf(stdout,"ssrc %x packet %d len %d:\n",sp->rtp_state_in.ssrc,sp->decoded_packets++,bytes);
		dump_frame(stdout,hdlc_frame,bytes);
		fflush(stdout);
		pthread_mutex_unlock(&Output_mutex);
	      }
	      struct rtp_header rtp_hdr;
	      memset(&rtp_hdr,0,sizeof(rtp_hdr));
	      rtp_hdr.version = 2;
	      rtp_hdr.type = AX25_PT;
	      rtp_hdr.seq = sp->rtp_state_out.seq++;
	      // RTP timestamp??
	      rtp_hdr.timestamp = sp->rtp_state_out.timestamp;
	      sp->rtp_state_out.timestamp += bytes;
	      rtp_hdr.ssrc = sp->rtp_state_out.ssrc;

	      unsigned char packet[2048],*dp;
	      dp = packet;
	      dp = hton_rtp(dp,&rtp_hdr);
	      memcpy(dp,hdlc_frame,bytes);
	      dp += bytes;
	      send(Output_fd,packet,dp - packet,0); // Check return code?
	      sp->rtp_state_out.packets++;
	      sp->rtp_state_out.bytes += bytes;
	    }
	  }
	  if(1 || frame_bit != 0){
	    memset(hdlc_frame,0,sizeof(hdlc_frame));
	    frame_bit = 0;
	  }
	  flagsync = 1;
	} else if(ones == 5){
	  // Drop stuffed zero
	} else if(ones < 5){
	  if(flagsync){
	    frame_bit++;
	  }
	}
	ones = 0;
      } else {
	// NRZI one
	if(++ones == 7){
	  // Abort
	  if(1 || frame_bit != 0){
	    memset(hdlc_frame,0,sizeof(hdlc_frame));
	    frame_bit = 0;
	  }
	  flagsync = 0;
	} else {
	  if(flagsync){
	    hdlc_frame[frame_bit/8] |= 1 << (frame_bit % 8);
	    frame_bit++;
	  }
	}
      }
      last_val = cur_val;
    }
    // Renormalize tone oscillators -- important when floats are used
    mark_phase /= cnrmf(mark_phase);
    space_phase /= cnrmf(space_phase);    
  }

  return NULL;

}


