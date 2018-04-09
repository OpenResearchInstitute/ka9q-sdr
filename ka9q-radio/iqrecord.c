// $Id: iqrecord.c,v 1.11 2018/02/06 11:46:44 karn Exp $
// Read complex float samples from stdin (e.g., from funcube.c)
// write into file
#define _GNU_SOURCE 1
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "radio.h"
#include "attr.h"
#include "multicast.h"

// One for each session being recorded
struct session {
  struct session *next;
  FILE *fp;                  // File being recorded
  uint32_t ssrc;             // RTP stream source ID
  uint32_t start_timestamp;  // first timestamp seen in stream
  uint32_t timestamp;        // next expected timestamp
  uint16_t seq;              // next expected sequence number
  double frequency;          // Tuner LO frequency
  struct sockaddr iq_sender; // Sender's IP address and source port
  unsigned int samprate;     // Nominal sampling rate indicated in packet header
};

struct session *Sessions;

// Largest Ethernet packet size
#define MAXPKT 1500

void closedown(int a);

void input_loop(void);


int Quiet;

struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;

int Input_fd;
char IQ_mcast_address_text[256] = "239.1.2.3"; // Default for testing



int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  seteuid(getuid());

  int c;
  char *locale;

  locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  // Defaults

  Quiet = 0;
  while((c = getopt(argc,argv,"I:l:q")) != EOF){
    switch(c){
    case 'I':
      strlcpy(IQ_mcast_address_text,optarg,sizeof(IQ_mcast_address_text));
      break;
    case 'l':
      locale = optarg;
      break;
    case 'q':
      Quiet++; // Suppress display
      break;
    default:
      fprintf(stderr,"Usage: %s [-I iq multicast address] [-l locale] [-q]\n",argv[0]);
      fprintf(stderr,"Default: %s -I %s -l %s\n",
	      argv[0],IQ_mcast_address_text,locale);
      exit(1);
      break;
    }
  }
  if(!Quiet){
    fprintf(stderr,"I/Q raw signal recorder for the Funcube Pro and Pro+\n");
    fprintf(stderr,"Copyright 2016 by Phil Karn, KA9Q; may be used under the terms of the GNU General Public License\n");
    fprintf(stderr,"Compiled %s on %s\n",__TIME__,__DATE__);
  }

  if(strlen(IQ_mcast_address_text) == 0){
    fprintf(stderr,"Specify -I IQ_mcast_address_text_address\n");
    exit(1);
  }
  setlocale(LC_ALL,locale);


  // Set up input socket for multicast data stream from front end
  Input_fd = setup_mcast(IQ_mcast_address_text,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up I/Q input\n");
    exit(1);
  }
  int n;
  n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  input_loop(); // Doesn't return

  exit(0);
}

void closedown(int a){
  if(!Quiet)
    fprintf(stderr,"iqrecord: caught signal %d: %s\n",a,strsignal(a));

  exit(1);
}

// Read from RTP network socket, assemble blocks of samples
void input_loop(){
  int cnt;
  char samples[MAXPKT];
  struct rtp_header rtp;
  struct status status;
  struct iovec iovec[3];

  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = &status;
  iovec[1].iov_len = sizeof(status);
  iovec[2].iov_base = samples;
  iovec[2].iov_len = sizeof(samples);
  
  struct msghdr message;
  message.msg_name = &Sender;
  message.msg_namelen = sizeof(Sender);
  message.msg_iov = iovec;
  message.msg_iovlen = sizeof(iovec) / sizeof(struct iovec);
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;

  char filename[PATH_MAX] = "";
  while(1){
    // Receive I/Q data from front end
    cnt = recvmsg(Input_fd,&message,0);
    if(cnt <= 0){    // ??
      perror("recvfrom");
      usleep(50000);
      continue;
    }
    if(cnt < sizeof(rtp) + sizeof(status))
      continue; // Too small, ignore
      
    // Host byte order
    rtp.ssrc = ntohl(rtp.ssrc);
    rtp.seq = ntohs(rtp.seq);
    rtp.timestamp = ntohl(rtp.timestamp);
    
    struct session *sp;
    for(sp = Sessions;sp != NULL;sp=sp->next){
      if(sp->ssrc == rtp.ssrc
	 && memcmp(&sp->iq_sender,&Sender,sizeof(sp->iq_sender)) == 0
	 && sp->frequency == status.frequency){
	break;
      }
    }
    if(sp == NULL){ // Not found; create new one
      sp = calloc(1,sizeof(*sp));

      // Initialize entry
      sp->next = Sessions;
      Sessions = sp;

      memcpy(&sp->iq_sender,&Sender,sizeof(sp->iq_sender));
      sp->ssrc = rtp.ssrc;
      sp->seq = rtp.seq;
      sp->start_timestamp = rtp.timestamp;
      sp->timestamp = sp->start_timestamp;
      sp->frequency = status.frequency;

      // Create file with name iqrecord-frequency-ssrc
      snprintf(filename,sizeof(filename),"iqrecord-%.1lfHz-%lx",status.frequency,(long unsigned)rtp.ssrc);
      sp->fp = fopen(filename,"w+");

      if(sp->fp == NULL){
	fprintf(stderr,"can't write file %s\n",filename);
	perror("open");
	continue;
      }
      if(!Quiet)
	fprintf(stderr,"creating file %s\n",filename);

      char *iobuffer = malloc(4096);   // One page
      setbuffer(sp->fp,iobuffer,4096); // Should free(iobuffer) after the file is closed

      int const fd = fileno(sp->fp);
      fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data
      attrprintf(fd,"samplerate","%lu",(unsigned long)status.samprate);
      attrprintf(fd,"frequency","%.1f",status.frequency);
      attrprintf(fd,"ssrc","%lx",(long unsigned)rtp.ssrc);

      char sender_text[NI_MAXHOST];
      getnameinfo((struct sockaddr *)&Sender,sizeof(Sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
      attrprintf(fd,"source","%s",sender_text);
      attrprintf(fd,"multicast","%s",IQ_mcast_address_text);
      
      struct timeval tv;
      gettimeofday(&tv,NULL);
      attrprintf(fd,"unixstarttime","%ld.%06ld",(long)tv.tv_sec,(long)tv.tv_usec);
      
      // 2 channels, I & Q
      attrprintf(fd,"channels","2");

      // Signed 16-bit integers, little-endian (Intel)
      attrprintf(fd,"sampleformat","s16le");
    }

    if(rtp.seq != sp->seq || rtp.timestamp != sp->timestamp){
      if(!Quiet)
	fprintf(stderr,"iqrecord %s: Expected seq %d, got %d; expected timestamp %u, got %u\n",
		filename,sp->seq,rtp.seq,sp->timestamp,rtp.timestamp);
      sp->seq = rtp.seq;
      sp->timestamp = rtp.timestamp;
    }
    // Should I limit the range on this?
    fseek(sp->fp,2 * sizeof(int16_t) * (rtp.timestamp - sp->start_timestamp),SEEK_SET);

    cnt -= sizeof(rtp) + sizeof(status);
    fwrite(samples,sizeof(*samples),cnt,sp->fp);
    sp->seq++;
    sp->timestamp += cnt/4; // Assumes 2 channels of s16
  }
}
 
