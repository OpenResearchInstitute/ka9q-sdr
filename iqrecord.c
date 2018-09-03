// $Id: iqrecord.c,v 1.19 2018/08/29 01:34:15 karn Exp $
// Read and record complex I/Q stream or PCM baseband audio
// This version reverts to file I/O from an unsuccessful experiment to use mmap()
// Copyright 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>


#include "radio.h"
#include "attr.h"
#include "multicast.h"

// Largest Ethernet packet
// Normally this would be <1500,
// but what about Ethernet interfaces that can reassemble fragments?
// 65536 should be safe since that's the largest IPv4 datagram.
// But what about IPv6?
#define MAXPKT 65535

// size of stdio buffer for disk I/O
// This should be large to minimize write calls, but how big?
#define BUFFERSIZE (1<<20)

// One for each session being recorded
struct session {
  struct session *next;
  struct sockaddr iq_sender;   // Sender's IP address and source port

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;
  
  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO or IQ)
  long long source_timestamp;  // Timestamp from status header (IQ only)
  double frequency;            // Tuner LO frequency (IQ only)
  unsigned int samprate;       // Nominal sampling rate (explicit in IQ, implicitly 48 kHz in PCM)

  FILE *fp;                    // File being recorded
  void *iobuffer;
};

int Quiet;
double Duration = INFINITY;
char IQ_mcast_address_text[256];
struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;
int Input_fd;
struct session *Sessions;


void closedown(int a);
void input_loop(void);

void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session *next_s = Sessions->next;
    fflush(Sessions->fp);
    fclose(Sessions->fp);
    Sessions->fp = NULL;
    free(Sessions->iobuffer);
    Sessions->iobuffer = NULL;
    free(Sessions);
    Sessions = next_s;
  }
}

int main(int argc,char *argv[]){
#if 0 // Better done manually or in systemd?
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");
#endif
  char *locale;
  locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  // Defaults
  Quiet = 0;
  int c;
  while((c = getopt(argc,argv,"I:l:qd:")) != EOF){
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
    case 'd':
      Duration = strtod(optarg,NULL);
      break;
    default:
      fprintf(stderr,"Usage: %s -I iq multicast address [-l locale] [-q]\n",argv[0]);
      exit(1);
      break;
    }
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
  int n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  atexit(cleanup);

  input_loop(); // Doesn't return

  exit(0);
}

void closedown(int a){
  if(!Quiet)
    fprintf(stderr,"iqrecord: caught signal %d: %s\n",a,strsignal(a));

  exit(1);  // Will call cleanup()
}

// Read from RTP network socket, assemble blocks of samples
void input_loop(){
  char filename[PATH_MAX];
  memset(filename,0,sizeof(filename));

  double t = 0;

  while(!isfinite(Duration) || t < Duration){
    // Receive I/Q data from front end
    unsigned char buffer[MAXPKT];
    socklen_t socksize = sizeof(Sender);
    int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&Sender,&socksize);
    if(size <= 0){    // ??
      perror("recvfrom");
      usleep(50000);
      continue;
    }
    if(size < RTP_MIN_SIZE)
      continue; // Too small for RTP, ignore

    unsigned char *dp = buffer;
    struct rtp_header rtp;
    dp = ntoh_rtp(&rtp,dp);
    if(rtp.pad){
      // Remove padding
      size -= dp[size-1];
      rtp.pad = 0;
    }

    // I/Q status header (if present) is in host byte order
    struct status status;
    if(rtp.type == IQ_PT)
      dp = ntoh_status(&status,dp);
    else
      memset(&status,0,sizeof(status));

    signed short *samples = (signed short *)dp;
    size -= (dp - buffer);

    struct session *sp;
    for(sp = Sessions;sp != NULL;sp=sp->next){
      if(sp->ssrc == rtp.ssrc
	 && rtp.type  == sp->type
	 && memcmp(&sp->iq_sender,&Sender,sizeof(sp->iq_sender)) == 0
	 && (rtp.type != IQ_PT || sp->frequency == status.frequency)){
	break;
      }
    }
    if(sp == NULL){ // Not found; create new one
      sp = calloc(1,sizeof(*sp));

      // Initialize entry
      sp->next = Sessions;
      Sessions = sp;

      memcpy(&sp->iq_sender,&Sender,sizeof(sp->iq_sender));
      sp->type = rtp.type;
      sp->ssrc = rtp.ssrc;

      switch(sp->type){
      case PCM_MONO_PT:
	sp->channels = 1;
	sp->samprate = 48000;
	sp->frequency = 0; // Not applicable
	break;
      case PCM_STEREO_PT:
	sp->channels = 2;
	sp->samprate = 48000;
	sp->frequency = 0; // Not applicable
	break;
      case IQ_PT:
	sp->channels = 2;
	sp->frequency = status.frequency;
	sp->samprate = status.samprate;
	sp->source_timestamp = status.timestamp; // Timestamp from IQ status header
	break;
      }

      // Create file with name iqrecord-frequency-ssrc or pcmrecord-ssrc
      int suffix;
      for(suffix=0;suffix<100;suffix++){
	struct stat statbuf;

	if(status.frequency)
	  snprintf(filename,sizeof(filename),"iqrecord-%.1lfHz-%lx-%d",sp->frequency,(long unsigned)sp->ssrc,suffix);
	else
	  snprintf(filename,sizeof(filename),"pcmrecord-%lx-%d",(long unsigned)sp->ssrc,suffix);
	if(stat(filename,&statbuf) == -1 && errno == ENOENT)
	  break;
      }
      if(suffix == 100){
	fprintf(stderr,"Can't generate filename %s to write\n",filename);
	// After this many tries, something is probably seriously wrong
	exit(1);
      }
      sp->fp = fopen(filename,"w+");

      if(sp->fp == NULL){
	fprintf(stderr,"can't write file %s\n",filename);
	perror("open");
	continue;
      }
      if(!Quiet)
	fprintf(stderr,"creating file %s\n",filename);

      sp->iobuffer = malloc(BUFFERSIZE);
      setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);

      int const fd = fileno(sp->fp);
      fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data

      attrprintf(fd,"samplerate","%lu",(unsigned long)sp->samprate);
      attrprintf(fd,"channels","%d",sp->channels);
      attrprintf(fd,"ssrc","%lx",(long unsigned)rtp.ssrc);

      switch(sp->type){
      case IQ_PT:
	attrprintf(fd,"sampleformat","s16le");
	attrprintf(fd,"frequency","%.3lf",sp->frequency);
	attrprintf(fd,"source_timestamp","%lld",sp->source_timestamp);
	break;
      case PCM_MONO_PT:
      case PCM_STEREO_PT:
	attrprintf(fd,"sampleformat","s16be");
	break;
      case OPUS_PT: // No support yet; should put in container
	break;
      }

      char sender_text[NI_MAXHOST];
      // Don't wait for an inverse resolve that might cause us to lose data
      getnameinfo((struct sockaddr *)&Sender,sizeof(Sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
      attrprintf(fd,"source","%s",sender_text);
      attrprintf(fd,"multicast","%s",IQ_mcast_address_text);
      
      struct timeval tv;
      gettimeofday(&tv,NULL);
      attrprintf(fd,"unixstarttime","%ld.%06ld",(long)tv.tv_sec,(long)tv.tv_usec);
    }
    int sample_count = size / (sizeof(*samples) * sp->channels);
    off_t offset = rtp_process(&sp->rtp_state,&rtp,sample_count);

    // The seek offset relative to the current position in the file is the signed (modular) difference between
    // the actual and expected RTP timestamps. This should automatically handle
    // 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz

    // Should I limit the range on this?

    if(offset)
      fseeko(sp->fp,offset,SEEK_CUR);
    fwrite(samples,1,size,sp->fp);
    t += (double)sample_count / sp->samprate;
  }
}
 
