// $Id: aprsfeed.c,v 1.20 2018/09/08 08:59:13 karn Exp $
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
#include <ctype.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"

char *Mcast_address_text = "ax25.mcast.local";
char *Host = "noam.aprs2.net";
char *Port = "14580";
char *User;
char *Passcode;
char *Logfilename;
FILE *Logfile;
int Verbose;
int Mcast_ttl = 0;

int Input_fd = -1;
int Network_fd = -1;

void *netreader(void *arg);

int main(int argc,char *argv[]){
  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    fprintf(stderr,"seteuid: %s\n",strerror(errno));
  
  setlocale(LC_ALL,getenv("LANG"));
  setlinebuf(stdout);

  int c;
  while((c = getopt(argc,argv,"u:p:I:vh:f:")) != EOF){
    switch(c){
    case 'f':
      Logfilename = optarg;
      Verbose = 0;
      break;
    case 'u':
      User = optarg;
      break;
    case 'v':
      if(!Logfilename)
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
      fprintf(stderr,"Usage: %s -u user [-p passcode] [-v] [-I mcast_address][-h host]\n",argv[0]);
      exit(1);
    }
  }
  // Set up multicast input
  if((Input_fd = setup_mcast(Mcast_address_text,0,Mcast_ttl,0)) == -1){
    fprintf(stderr,"Can't set up multicast input from %s\n",Mcast_address_text);
    exit(1);
  }

  if(Logfilename)
    Logfile = fopen(Logfilename,"a");
  else if(Verbose)
    Logfile = stdout;

  if(Logfile){
    setlinebuf(Logfile);
    fprintf(Logfile,"APRS feeder program by KA9Q\n");
  }
  if(User == NULL){
    fprintf(stderr,"Must specify -u User\n");
    exit(1);
  }
  if(!Passcode){
    // Calculate trivial hash authenticator
    int hash = 0x73e2;
    char callsign[11];
    strncpy(callsign,User,sizeof(callsign)-1);
    char *cp;
    if((cp = strchr(callsign,'-')) != NULL)
      *cp = '\0';
    
    int len = strlen(callsign);

    for(int i=0; i<len; i += 2){
      hash ^= toupper(callsign[i]) << 8;
      hash ^= toupper(callsign[i+1]);
    }
    hash &= 0x7fff;
    if(asprintf(&Passcode,"%d",hash) < 0){
      fprintf(stderr,"Unexpected error in computing passcode\n");
      exit(1);
    }
  }

  {
  struct addrinfo hints;
  memset(&hints,0,sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_CANONNAME|AI_ADDRCONFIG;

  struct addrinfo *results = NULL;
  int ecode;
  // Try a few times in case we come up before the resolver is quite ready
  for(int tries=0; tries < 10; tries++){
    if((ecode = getaddrinfo(Host,Port,&hints,&results)) == 0)
      break;
    usleep(500000);
  } 
  if(ecode != 0){
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
  freeaddrinfo(results);
  if(Logfile)
    fprintf(Logfile,"Connected to APRS server %s port %s\n",resp->ai_canonname,Port);

  }


  FILE *network = fdopen(Network_fd,"w+");
  setlinebuf(network);

  pthread_t read_thread;
  pthread_create(&read_thread,NULL,netreader,NULL);

  // Log into the network
  fprintf(network,"user %s pass %s vers KA9Q-aprs 1.0\r\n",User,Passcode);
  // Check for error return here
  
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
    time_t t;
    struct tm *tmp;
    time(&t);
    tmp = gmtime(&t);
    if(Logfile){
      fprintf(Logfile,"%d %s %04d %02d:%02d:%02d UTC ssrc %x seq %d",tmp->tm_mday,Months[tmp->tm_mon],tmp->tm_year+1900,
	      tmp->tm_hour,tmp->tm_min,tmp->tm_sec,rtp_header.ssrc,rtp_header.seq);
    }

    // Parse incoming AX.25 frame
    struct ax25_frame frame;
    if(ax25_parse(&frame,dp,pktlen) < 0){
      if(Logfile)
	fprintf(Logfile," Unparsable packet\n");
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
    if(Logfile)
      fprintf(Logfile," %s\n",monstring);

    if(frame.control != 0x03 || frame.type != 0xf0){
      if(Logfile)
	fprintf(Logfile," Not relaying: invalid ax25 ctl/protocol\n");
      continue;
    }
    if(infolen == 0){
      if(Logfile)
	fprintf(Logfile," Not relaying: empty I field\n");
      continue;
    }
    if(is_tcpip){
      if(Logfile)
	fprintf(Logfile," Not relaying: Internet relayed packet\n");
      continue;
    }
    if(frame.information[0] == '{'){
      if(Logfile)
	fprintf(Logfile," Not relaying: third party traffic\n");	
      continue;
    }

    // Send to APRS network with appended crlf
    fprintf(network,"%s\r\n",monstring);
  }
}

// Just read and echo responses from server
void *netreader(void *arg){
  pthread_setname("aprs-read");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  // Create our own stream; there seem to be problems sharing a common stream among threads
  FILE *network = fdopen(Network_fd,"r");

  while((linelen = getline(&line,&linecap,network)) > 0){
    if(Logfile)
      fwrite(line,linelen,1,Logfile);
  }
  free(line); line = NULL;
  return NULL;
}
