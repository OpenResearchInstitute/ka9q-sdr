// $Id: aprsfeed.c,v 1.8 2018/07/31 11:25:32 karn Exp $
// Process AX.25 frames containing APRS data, feed to APRS2 network
// Copyright 2018, Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"

char *Mcast_address_text = "ax25.mcast.local";
char *Host = "noam.aprs2.net";
char *Port = "14580";
char *User;
char *Passcode;

int Verbose;
int Input_fd = -1;
int Network_fd = -1;

void *netreader(void *arg);

int main(int argc,char *argv[]){
  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt(argc,argv,"u:p:I:vh:")) != EOF){
    switch(c){
    case 'u':
      User = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    case 'h':
      Host = optarg;
      break;
    case 'p':
      Passcode = optarg;
      break;
    case 'I':
      Mcast_address_text = optarg;
      break;
    default:
      fprintf(stderr,"Usage: %s -u user -p passcode [-v] [-I mcast_address][-h host]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -I %s -h %s\n",argv[0],Mcast_address_text,Host);
      exit(1);
    }
  }
  if(Verbose)
    fprintf(stderr,"APRS feeder program by KA9Q\n");
  if(User == NULL || Passcode == NULL){
    fprintf(stderr,"Must specify -u User -p passcode\n");
    exit(1);
  }

  {
  struct addrinfo hints;
  memset(&hints,0,sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_CANONNAME|AI_ADDRCONFIG;

  if(Verbose)
    fprintf(stderr,"APRS server: %s:%s\n",Host,Port);
  struct addrinfo *results = NULL;
  int ecode;
  if((ecode = getaddrinfo(Host,Port,&hints,&results)) != 0){
    fprintf(stderr,"Can't getaddrinfo(%s,%s): %s\n",Host,Port,gai_strerror(ecode));
    exit(1);
  }
  struct addrinfo *resp;
  for(resp = results; resp != NULL; resp = resp->ai_next){
    if((Network_fd = socket(resp->ai_family,resp->ai_socktype,resp->ai_protocol)) < 0)
      continue;
    if(connect(Network_fd,resp->ai_addr,resp->ai_addrlen) == 0)
      break;
    close(Network_fd); Network_fd = -1;
  }
  if(resp == NULL){
    fprintf(stderr,"Can't connect to server %s:%s\n",Host,Port);
    exit(1);
  }
  if(Verbose)
    fprintf(stderr,"Connected to server %s port %s\n",
	    resp->ai_canonname,Port);
  freeaddrinfo(results);
  }
  
  pthread_t read_thread;
  if(Verbose)
    pthread_create(&read_thread,NULL,netreader,NULL);

  // Log into the network
  {
  char *message;
  int mlen;
  mlen = asprintf(&message,"user %s pass %s vers KA9Q-aprs 1.0\r\n",User,Passcode);
  if(write(Network_fd,message,mlen) != mlen){
    perror("Login write to network failed");
    exit(1);
  }
  free(message);
  }
  
  // Set up multicast input
  Input_fd = setup_mcast(Mcast_address_text,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up input from %s\n",
	    Mcast_address_text);
    exit(1);
  }
  unsigned char packet[2048];
  int pktlen;

  while((pktlen = recv(Input_fd,packet,sizeof(packet),0)) > 0){
    struct rtp_header rtp_header;
    unsigned char *dp = packet;

    dp = ntoh_rtp(&rtp_header,dp);
    pktlen -= dp - packet;

    if(rtp_header.type != AX25_PT)
      continue; // Wrong type

    // Emit local timestamp
    if(Verbose){
      time_t t;
      struct tm *tmp;
      time(&t);
      tmp = gmtime(&t);
      fprintf(stderr,"%d %s %04d %02d:%02d:%02d UTC",tmp->tm_mday,Months[tmp->tm_mon],tmp->tm_year+1900,
	      tmp->tm_hour,tmp->tm_min,tmp->tm_sec);
      fprintf(stderr," ssrc %x seq %d",rtp_header.ssrc,rtp_header.seq);
    }

    // Parse incoming AX.25 frame
    struct ax25_frame frame;
    if(ax25_parse(&frame,dp,pktlen) < 0){
      if(Verbose)
	fprintf(stderr," Unparsable packet\n");
      continue;
    }
		
    // Construct TNC2-style monitor string for APRS reporting
    char monstring[2048]; // Should be large enough for any legal AX.25 frame; we'll assert this periodically
    int sspace = sizeof(monstring);
    int infolen = 0;
    int is_tcpip = 0;
    {
      memset(monstring,0,sizeof(monstring));
      char *cp = monstring;
      {
	int w = snprintf(cp,sspace,"%s>%s",frame.source,frame.dest);
	cp += w; sspace -= w;
	assert(sspace > 0);
      }
      for(int i=0;i<frame.ndigi;i++){
	// if "TCPIP" appears, this frame came off the Internet and should not be sent back to it
	if(strcmp(frame.digipeaters[i].name,"TCPIP") == 0)
	  is_tcpip = 1;
	int w = snprintf(cp,sspace,",%s%s",frame.digipeaters[i].name,frame.digipeaters[i].h ? "*" : "");
	cp += w; sspace -= w;
	assert(sspace > 0);
      }
      {
	// qAR means a bidirectional i-gate, qAO means receive-only
	//    w = snprintf(cp,sspace,",qAR,%s",User);
	int w = snprintf(cp,sspace,",qAO,%s",User);
	cp += w; sspace -= w;
	*cp++ = ':'; sspace--;
	assert(sspace > 0);
      }      
      for(int i=0; i < frame.info_len; i++){
	char c = frame.information[i] & 0x7f; // Strip parity in monitor strings
	if(c != '\r' && c != '\n' && c != '\0'){
	  // Strip newlines, returns and nulls (we'll add a cr-lf later)
	  *cp++ = c;
	  sspace--;
	  infolen++;
	  assert(sspace > 0);
	}
      }
      *cp++ = '\0';
      sspace--;
    }      
    assert(sizeof(monstring) - sspace - 1 == strlen(monstring));
    if(Verbose)
      fprintf(stderr," %s\n",monstring);
    if(frame.control != 0x03 || frame.type != 0xf0){
      if(Verbose)
	fprintf(stderr," Not relaying: invalid ax25 ctl/protocol\n");
      continue;
    }
    if(infolen == 0){
      if(Verbose)
	fprintf(stderr," Not relaying: empty I field\n");
      continue;
    }
    if(is_tcpip){
      if(Verbose)
	fprintf(stderr," Not relaying: Internet relayed packet\n");
      continue;
    }
    if(frame.information[0] == '{'){
      if(Verbose)
	fprintf(stderr," Not relaying: third party traffic\n");	
      continue;
    }

    // Send to APRS network with appended crlf
    {
      assert(sspace >= 2);
      int len = strlen(monstring);
      char *cp = monstring + len;
      *cp++ = '\r';
      *cp++ = '\n';
      len += 2;
      if(write(Network_fd,monstring,len) != len){
	perror(" network report write");
	break;
      }
    }
  }
}

// Just read and echo responses from server
void *netreader(void *arg){
  pthread_setname("aprs-read");

  while(1){
    char c;
    int r = read(Network_fd,&c,1);
    if(r < 0)
      break;
    if(write(1,&c,1) != 1){
      perror("server echo write");
      break;
    }
  }
  return NULL;
}
