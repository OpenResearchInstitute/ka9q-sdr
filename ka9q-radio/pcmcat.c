// $Id: pcmcat.c,v 1.2 2018/02/22 06:52:04 karn Exp $
// Receive and stream PCM RTP data to stdout

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#include "multicast.h"

struct pcmstream {
  struct pcmstream *prev;       // Linked list pointers
  struct pcmstream *next; 
  uint32_t ssrc;            // RTP Sending Source ID
  int eseq;                 // Next expected RTP sequence number
  int etime;                // Next expected RTP timestamp
  int type;                 // RTP type (10,11,20)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port


  unsigned long age;
  unsigned long rtp_packets;    // RTP packets for this session
  unsigned long drops;      // Apparent rtp packet drops
  unsigned long invalids;   // Unknown RTP type
  unsigned long empties;    // RTP but no data
  unsigned long dupes;      // Duplicate or old serial numbers
};



char *Mcast_address_text = "audio-pcm-mcast.local";
int const Bufsize = 2048;
float const Samprate = 48000;

int Input_fd = -1;
struct pcmstream *Pcmstream;
int Verbose;

struct pcmstream *lookup_session(const struct sockaddr *sender,const uint32_t ssrc){
  struct pcmstream *sp;
  for(sp = Pcmstream; sp != NULL; sp = sp->next){
    if(sp->ssrc == ssrc && memcmp(&sp->sender,sender,sizeof(*sender)) == 0){
      // Found it
      if(sp->prev != NULL){
	// Not at top of bucket chain; move it there
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;

	sp->prev->next = sp->next;
	sp->prev = NULL;
	sp->next = Pcmstream;
	Pcmstream = sp;
      }
      return sp;
    }
  }
  return NULL;
}
// Create a new session, partly initialize
struct pcmstream *make_session(struct sockaddr const *sender,uint32_t ssrc,uint16_t seq,uint32_t timestamp){
  struct pcmstream *sp;

  if((sp = calloc(1,sizeof(*sp))) == NULL)
    return NULL; // Shouldn't happen on modern machines!
  
  // Initialize entry
  memcpy(&sp->sender,sender,sizeof(struct sockaddr));
  sp->ssrc = ssrc;
  sp->eseq = seq;
  sp->etime = timestamp;

  // Put at head of bucket chain
  sp->next = Pcmstream;
  if(sp->next != NULL)
    sp->next->prev = sp;
  Pcmstream = sp;
  return sp;
}

int close_session(struct pcmstream *sp){
  if(sp == NULL)
    return -1;
  
  // Remove from linked list
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Pcmstream = sp->next;
  free(sp);
  return 0;
}



int main(int argc,char *argv[]){
  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt(argc,argv,"I:v")) != EOF){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'I':
      Mcast_address_text = optarg;
      break;
    default:
      fprintf(stderr,"Usage: %s [-v] [-I mcast_address]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -I %s\n",argv[0],Mcast_address_text);
      exit(1);
    }
  }

  // Set up multicast input
  Input_fd = setup_mcast(Mcast_address_text,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up input from %s\n",
	    Mcast_address_text);
    exit(1);
  }
  struct iovec iovec[2];
  struct rtp_header rtp;
  signed short data[Bufsize];
  
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = data;
  iovec[1].iov_len = sizeof(data);

  struct msghdr message;
  struct sockaddr sender;
  message.msg_name = &sender;
  message.msg_namelen = sizeof(sender);
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;

  // audio input thread
  // Receive audio multicasts, multiplex into sessions, send to output
  // What do we do if we get different streams?? think about this
  while(1){
    int size;

    size = recvmsg(Input_fd,&message,0);
    if(size == -1){
      if(errno != EINTR){ // Happens routinely
	perror("recvmsg");
	usleep(1000);
      }
      continue;
    }
    if(size < sizeof(rtp)){
      usleep(500); // Avoid tight loop
      continue; // Too small to be valid RTP
    }
    // To host order
    rtp.ssrc = ntohl(rtp.ssrc);
    rtp.seq = ntohs(rtp.seq);
    rtp.timestamp = ntohl(rtp.timestamp);

    if(rtp.mpt != 10 && rtp.mpt != 20 && rtp.mpt != 11) // 1 byte, no need to byte swap
      goto endloop; // Discard unknown RTP types to avoid polluting session table


    struct pcmstream *sp = lookup_session(&sender,rtp.ssrc);
    if(sp == NULL){
      // Not found
      if((sp = make_session(&sender,rtp.ssrc,rtp.seq,rtp.timestamp)) == NULL){
	fprintf(stderr,"No room for new session!!\n");
	goto endloop;
      }
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		  //		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);
      sp->dupes = 0;
      sp->age = 0;
      if(Verbose)
	fprintf(stderr,"New session from %s, ssrc %x\n",sp->addr,sp->ssrc);
    }
    sp->age = 0;
    int drop = 0;

    sp->rtp_packets++;
    if(rtp.seq != sp->eseq){
      int const diff = (int)(rtp.seq - sp->eseq);
      if(Verbose > 1)
	fprintf(stderr,"ssrc %lx: expected %d got %d\n",(unsigned long)rtp.ssrc,sp->eseq,rtp.seq);
      if(diff < 0 && diff > -10){
	sp->dupes++;
	goto endloop;	// Drop probable duplicate
      }
      drop = diff; // Apparent # packets dropped
      sp->drops += abs(drop);
      // Should probably emit 0 padding here
    }
    sp->eseq = (rtp.seq + 1) & 0xffff;

    sp->type = rtp.mpt;
    size -= sizeof(rtp); // Bytes in payload
    if(size <= 0){
      sp->empties++;
      goto endloop; // empty?!
    }
    int samples = 0;

    switch(rtp.mpt){
    case 11: // Mono only for now
      samples = size / 2;
      signed short *dp = data;
      while(samples-- > 0){
	// Swap sample to host order, cat to stdout
	signed short d = ntohs(*dp++);
	putchar(d & 0xff);
	putchar((d >> 8) & 0xff);
      }
      break;
    default:
      samples = 0;
      break; // ignore
    }
    sp->etime = rtp.timestamp + samples;

  endloop:;
  }
  exit(0);
}



