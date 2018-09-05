// $Id: monitor.c,v 1.81 2018/09/05 08:18:22 karn Exp $
// Listen to multicast group(s), send audio to local sound device via portaudio
// Copyright 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <complex.h> // test
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <opus/opus.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netdb.h>
#include <portaudio.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>


#include "misc.h"
#include "multicast.h"

// Global config variables
#define SAMPRATE 48000        // Too hard to handle other sample rates right now
#define MAX_MCAST 20          // Maximum number of multicast addresses
#define PKTSIZE 16384         // Maximum bytes per RTP packet - must be bigger than Ethernet MTU (including offloaded reassembly)
#define SAMPPCALLBACK (SAMPRATE/50)     // 20 ms @ 48 kHz
#define BUFFERSIZE (1<<19)    // about 10.92 sec at 48 kHz stereo - must be power of 2!!

// Incoming RTP packets
struct packet {
  struct packet *next;
  struct rtp_header rtp;
  unsigned char *data;
  int len;
  unsigned char content[PKTSIZE];
};

struct session {
  struct session *prev;     // Linked list pointers
  struct session *next; 

  struct sockaddr_storage sender;
  char *dest;
  char src_addr[NI_MAXHOST];    // RTP Source IP address
  char src_port[NI_MAXSERV];    // RTP Source port

  char dest_addr[NI_MAXHOST];    // RTP Destination IP address
  char dest_port[NI_MAXSERV];    // RTP Destination port

  pthread_t task;           // Thread reading from queue and running decoder
  struct packet *queue;     // Incoming RTP packets
  pthread_mutex_t qmutex;   // Mutex protecting packet queue
  pthread_cond_t qcond;     // Condition variable for arrival of new packet

  struct rtp_state rtp_state;
  uint32_t ssrc;            // RTP Sending Source ID
  int type;                 // RTP type (10,11,20,111)

  OpusDecoder *opus;        // Opus codec decoder handle, if needed
  int opus_bandwidth;       // Opus stream audio bandwidth
  int channels;             // Channels (1 or 2)
  int frame_size;           // Samples in a frame
  float gain;               // Gain; 1 = 0 dB
  float pan;                // Stereo position: 0 = center; -1 = full left; +1 = full right

  unsigned long packets;    // RTP packets for this session
  unsigned long empties;    // RTP but no data

  long long wptr;           // Unwrapped write pointer, not wrapped (will wrap in 6 million years!)

  int terminate;
};

char *Mcast_address_text[MAX_MCAST]; // Multicast address(es) we're listening to
char Audiodev[256];           // Name of audio device; empty means portaudio's default
int Update_interval = 100;    // Default time in ms between display updates
int List_audio;               // List audio output devices and exit
int Verbose;                  // Verbosity flag (currently unused)
int Quiet;                    // Disable curses
int Nfds;                     // Number of streams
struct session *Session;      // Link to head of session structure chain
pthread_mutex_t Sess_mutex;

PaStream *Pa_Stream;          // Portaudio stream handle
int inDevNum;                 // Portaudio's audio output device index
float const SCALE = 1./SHRT_MAX;
WINDOW *Mainscr;
struct timeval Start_unix_time;
PaTime Start_pa_time;
struct session *Current;
pthread_t Display_task;
float Output_buffer[BUFFERSIZE][2]; // Decoded audio output, written by processing thread and read by PA callback
long long Rptr;                // Unwrapped read pointer (will overflow in 6 million years)

void cleanup(void){
  Pa_Terminate();
  if(!Quiet){
    echo();
    nocbreak();
    endwin();
  }
}

void closedown(int s){
  fprintf(stderr,"Signal %d, exiting\n",s);
  exit(0);
}


void closedown(int);
void *display(void *);
struct session *lookup_session(const struct sockaddr_storage *,uint32_t);
struct session *create_session(struct sockaddr_storage const *,uint32_t);
int close_session(struct session *);
static int pa_callback(const void *,void *,unsigned long,const PaStreamCallbackTimeInfo*,PaStreamCallbackFlags,void *);
void *decode_task(void *x);
void *sockproc(void *arg);

int main(int argc,char * const argv[]){
  // Try to improve our priority, then drop root
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 15);
  if(seteuid(getuid()) != 0)
    perror("seteuid");

  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt(argc,argv,"R:S:I:vLqu:")) != EOF){
    switch(c){
    case 'L':
      List_audio++;
      break;
    case 'R':
      strncpy(Audiodev,optarg,sizeof(Audiodev));
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      if(Nfds == MAX_MCAST){
	fprintf(stderr,"Too many multicast addresses; max %d\n",MAX_MCAST);
      } else 
	Mcast_address_text[Nfds++] = optarg;
      break;
    case 'q': // No ncurses
      Quiet++;
      break;
    case 'u':
      Update_interval = strtol(optarg,NULL,0);
      break;
    default:
      fprintf(stderr,"Usage: %s [-v] [-q] [-L] [-R audio device] -I mcast_address [-I mcast_address]\n",argv[0]);
      exit(1);
    }
  }
  PaError r = Pa_Initialize();
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    return r;
  }
  atexit(cleanup);

  if(List_audio){
    // On stdout, not stderr, so we can toss ALSA's noisy error messages
    printf("Audio devices:\n");
    int numDevices = Pa_GetDeviceCount();
    for(int inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      printf("%d: %s\n",inDevNum,deviceInfo->name);
    }
    exit(0);
  }
  if(Nfds == 0){
    fprintf(stderr,"At least one -I option required\n");
    exit(1);
  }

  char *nextp = NULL;
  int d;
  int numDevices = Pa_GetDeviceCount();
  if(strlen(Audiodev) == 0){
    // not specified; use default
    inDevNum = Pa_GetDefaultOutputDevice();
  } else if(d = strtol(Audiodev,&nextp,0),nextp != Audiodev && *nextp == '\0'){
    if(d >= numDevices){
      fprintf(stderr,"%d is out of range, use %s -L for a list\n",d,argv[0]);
      exit(1);
    }
    inDevNum = d;
  } else {
    for(inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      if(strcmp(deviceInfo->name,Audiodev) == 0)
	break;
    }
  }
  if(inDevNum == paNoDevice){
    fprintf(stderr,"Portaudio: no available devices\n");
    exit(1);
  }

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);

  signal(SIGTERM,closedown);
  signal(SIGHUP,closedown);
  signal(SIGPIPE,SIG_IGN);


  if(!Quiet)
    pthread_create(&Display_task,NULL,display,NULL);

  // Spawn one thread per address
  pthread_t sockthreads[Nfds];

  pthread_mutex_init(&Sess_mutex,NULL);

  for(int i=0; i<Nfds; i++)
    pthread_create(&sockthreads[i],NULL,sockproc,Mcast_address_text[i]);

  // Create portaudio stream.
  // Runs continuously, playing silence until audio arrives.
  // This allows multiple streams to be played on hosts that only support one
  PaStreamParameters outputParameters;
  memset(&outputParameters,0,sizeof(outputParameters));
  outputParameters.channelCount = 2;
  outputParameters.device = inDevNum;
  outputParameters.sampleFormat = paFloat32;
  outputParameters.suggestedLatency = 0.020; // 0 doesn't seem to be a good value on OSX, lots of underruns and stutters
  
  r = Pa_OpenStream(&Pa_Stream,
		    NULL,
		    &outputParameters,
		    SAMPRATE,
		    paFramesPerBufferUnspecified, // seems to be 31 on OSX
		    //SAMPPCALLBACK,
		    0,
		    pa_callback,
		    NULL);

  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));      
    exit(1);
  }

  // Do this at the last minute at startup since the upcall will come quickly
  r = Pa_StartStream(Pa_Stream);
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    exit(1);
  }
  Start_pa_time = Pa_GetStreamTime(Pa_Stream);
  gettimeofday(&Start_unix_time,NULL);

  while(1)
    usleep(5000000);

  echo();
  nocbreak();
  endwin();
  exit(0);
}

void *sockproc(void *arg){
  char *mcast_address_text = (char *)arg;

  // Set up multicast input
  int input_fd = setup_mcast(mcast_address_text,0,0);
  if(input_fd == -1){
    fprintf(stderr,"Can't set up input %s\n",mcast_address_text);
    pthread_exit(NULL);
  }
  struct packet *pkt = NULL;

  // Main loop begins here
  while(1){

    // Need a new packet buffer?
    if(!pkt)
      pkt = malloc(sizeof(*pkt));
    // Zero these out to catch any uninitialized derefs
    pkt->next = NULL;
    pkt->data = NULL;
    pkt->len = 0;
    
    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    int size = recvfrom(input_fd,&pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&sender,&socksize);
    
    if(size == -1){
      if(errno != EINTR){ // Happens routinely, e.g., when window resized
	perror("recvfrom");
	usleep(1000);
      }
      continue;  // Reuse current buffer
    }
    if(size <= RTP_MIN_SIZE)
      continue; // Must be big enough for RTP header and at least some data
    
    // Convert RTP header to host format
    unsigned char *dp = ntoh_rtp(&pkt->rtp,pkt->content);
    pkt->data = dp;
    pkt->len = size - (dp - pkt->content);
    if(pkt->rtp.pad){
      pkt->len -= dp[pkt->len-1];
      pkt->rtp.pad = 0;
    }
    if(pkt->len <= 0)
      continue; // Used to be an assert, but would be triggered by bogus packets
    
    // Find appropriate session; create new one if necessary
    struct session *sp = lookup_session(&sender,pkt->rtp.ssrc);
    if(!sp){
      // Not found
      if(!(sp = create_session(&sender,pkt->rtp.ssrc))){
	fprintf(stderr,"No room!!\n");
	continue;
      }
      sp->dest = mcast_address_text;
      
      pthread_mutex_init(&sp->qmutex,NULL);
      pthread_cond_init(&sp->qcond,NULL);
      if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	perror("pthread_create");
	close_session(sp);
	continue;
      }
    }
    
    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;
    pthread_mutex_lock(&sp->qmutex);
    for(qe = sp->queue; qe && pkt->rtp.seq >= qe->rtp.seq; q_prev = qe,qe = qe->next)
      ;
    
    pkt->next = qe;
    if(q_prev)
      q_prev->next = pkt;
    else
      sp->queue = pkt; // Front of list
    pkt = NULL;        // force new packet to be allocated
    // wake up decoder thread
    pthread_cond_signal(&sp->qcond);
    pthread_mutex_unlock(&sp->qmutex);
  }      
}




// Portaudio callback - transfer data (if any) to provided buffer
static int pa_callback(const void *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       const PaStreamCallbackTimeInfo* timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData){
  if(!outputBuffer)
    return paAbort; // can this happen??
  
  assert(framesPerBuffer < BUFFERSIZE/2); // Make sure ring buffer is big enough
  float *out = outputBuffer;
  for(int n=0; n < framesPerBuffer; n++){
    int rptr = Rptr++ & (BUFFERSIZE-1);
    *out++ = Output_buffer[rptr][0];
    *out++ = Output_buffer[rptr][1];
    Output_buffer[rptr][0] = 0;
    Output_buffer[rptr][1] = 0;
  }
  return paContinue;
}

void decode_task_cleanup(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  pthread_mutex_destroy(&sp->qmutex);
  pthread_cond_destroy(&sp->qcond);

  if(sp->opus){
    opus_decoder_destroy(sp->opus);
    sp->opus = NULL;
  }
  struct packet *pkt_next;
  for(struct packet *pkt = sp->queue; pkt; pkt = pkt_next){
    pkt_next = pkt->next;
    free(pkt);
  }
}

// Thread to decode incoming RTP packets for each stream
void *decode_task(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  pthread_setname("decode");
  pthread_cleanup_push(decode_task_cleanup,arg);

  sp->gain = 1;    // 0 dB by default
  sp->pan = 0;     // center by default
  sp->wptr = Rptr;

  // Main loop; run until asked to quit
  while(!sp->terminate){

    struct packet *pkt = NULL;
    // Wait for packet to appear on queue
    pthread_mutex_lock(&sp->qmutex);
    while(!sp->queue)
      pthread_cond_wait(&sp->qcond,&sp->qmutex);
    pkt = sp->queue;
    sp->queue = pkt->next;
    pkt->next = NULL;
    pthread_mutex_unlock(&sp->qmutex);

    sp->type = pkt->rtp.type;
    sp->packets++; // Count all packets, regardless of type
      
    int samples_skipped = rtp_process(&sp->rtp_state,&pkt->rtp,0); // get rid of last arg
    if(samples_skipped < 0)
      goto done; // old dupe? What if it's simply out of sequence?

    // Compute gains and delays for stereo imaging
    // -6dB for each channel in the center
    // when full to one side or the other, that channel is +6 dB and the other is -inf dB
    float left_gain = sp->gain * (1 - sp->pan)/2;
    float right_gain = sp->gain * (1 + sp->pan)/2;
    int left_delay = 0;
    int right_delay = 0;
    // Also delay less favored channel 1 ms max
    // This is really what drives source localization in humans
    if(sp->pan > 0)
      left_delay = round(sp->pan * .001 * SAMPRATE); // Delay left channel
    else if(sp->pan < 0)
      right_delay = round(-sp->pan * .001 * SAMPRATE); // Delay right channel

    assert(left_delay >= 0 && right_delay >= 0);

    if(samples_skipped > 0){
      // gap in PCM data
      if(pkt->rtp.marker || samples_skipped >= 3840){
	if(sp->opus)
	  opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder and catch up
      } else {
	// Short gap
 	if(sp->opus && sp->wptr >= Rptr){ // Don't do this if it's already too late
	  // Decode any FEC, otherwise interpolate or create comfort noise
	  float bounce[samples_skipped][2]; // pick a better number
	  int samples = opus_decode_float(sp->opus,pkt->data,pkt->len,&bounce[0][0],samples_skipped,1);
	  assert(samples <= samples_skipped);
	  int left = sp->wptr + left_delay;
	  int right = sp->wptr + right_delay;
	  for(int i=0; i<samples; i++){
	    Output_buffer[left++ & (BUFFERSIZE-1)][0] += bounce[i][0] * left_gain;
	    Output_buffer[right++ & (BUFFERSIZE-1)][1] += bounce[i][1] * right_gain;
	  }
	}
      }
      sp->wptr += samples_skipped;	  // Short loss; pad with implicit zeroes
    }

    // Catch up to any overrun by adding playout delay
    if(sp->wptr < Rptr)
      sp->wptr = Rptr;      // Underrun, catch up

    // Decode frame, add into output buffer
    int left = sp->wptr + left_delay;
    int right = sp->wptr + right_delay;
    signed short *data_ints = (signed short *)&pkt->data[0];	

    switch(pkt->rtp.type){
    case PCM_STEREO_PT:
      sp->channels = 2;
      sp->frame_size = pkt->len / 4; // Number of stereo samples
      for(int i=0; i < sp->frame_size; i++){
	Output_buffer[left++ & (BUFFERSIZE-1)][0] += SCALE * (signed short)ntohs(*data_ints++) * left_gain;
	Output_buffer[right++ & (BUFFERSIZE-1)][1] += SCALE * (signed short)ntohs(*data_ints++) * right_gain;
      }
      break;
    case PCM_MONO_PT:
      sp->channels = 1;
      sp->frame_size = pkt->len / 2; // Number of stereo samples
      for(int i=0; i < sp->frame_size; i++){
	float s = SCALE * (signed short)ntohs(*data_ints++);
	Output_buffer[left++ & (BUFFERSIZE-1)][0] += s * left_gain;
	Output_buffer[right++ & (BUFFERSIZE-1)][1] += s * right_gain;
      }
      break;
    case OPUS_PT:
    case 20:
      sp->channels = 2;
      sp->frame_size = opus_packet_get_nb_samples(pkt->data,pkt->len,SAMPRATE);
      sp->opus_bandwidth = opus_packet_get_bandwidth(pkt->data);
      
      if(!sp->opus){
	int error;
	sp->opus = opus_decoder_create(SAMPRATE,2,&error);
	assert(sp->opus);
      }
      {
	float bounce[sp->frame_size][2];
	int samples = opus_decode_float(sp->opus,pkt->data,pkt->len,&bounce[0][0],sp->frame_size,0);
	assert(samples <= sp->frame_size);
	for(int i=0; i<samples; i++){
	  Output_buffer[left++ & (BUFFERSIZE-1)][0] += bounce[i][0] * left_gain;
	  Output_buffer[right++ & (BUFFERSIZE-1)][1] += bounce[i][1] * right_gain;
	}
      }
      break;
    default:
      sp->channels = 0;
      sp->frame_size = 0;
      break;
    }
    sp->wptr += sp->frame_size;
    sp->rtp_state.timestamp = pkt->rtp.timestamp + sp->frame_size;
  done:;
    free(pkt); pkt = NULL;
  }
  pthread_cleanup_pop(1);
  return NULL;
}
// Use ncurses to display streams
void *display(void *arg){

  pthread_setname("display");
  initscr();
  keypad(stdscr,TRUE);
  timeout(Update_interval);
  cbreak();
  noecho();
  
  Mainscr = stdscr;

  while(1){

    if(!Current)
      Current = Session;

    int row = 2;
    wmove(Mainscr,row,0);
    wclrtobot(Mainscr);

    wmove(Mainscr,row,0);

    mvwprintw(Mainscr,row++,0,"Type        ch BW Gain   Pan      SSRC     Queue Source/Dest");
    for(struct session *sp = Session; sp; sp = sp->next){
      int bw = 0; // Audio bandwidth (not bitrate) in kHz
      char *type,typebuf[30];
      switch(sp->type){
     case PCM_STEREO_PT:
      case PCM_MONO_PT:
	type = "PCM";
	bw = SAMPRATE / 2000;
	break;
      case 20: // for temporary backward compatibility
      case OPUS_PT:
	switch(sp->opus_bandwidth){
	case OPUS_BANDWIDTH_NARROWBAND:
	  bw = 4;
	  break;
	case OPUS_BANDWIDTH_MEDIUMBAND:
	  bw = 6;
	  break;
	case OPUS_BANDWIDTH_WIDEBAND:
	  bw = 8;
	  break;
	case OPUS_BANDWIDTH_SUPERWIDEBAND:
	  bw = 12;
	  break;
	case OPUS_BANDWIDTH_FULLBAND:
	  bw = 20;
	  break;
	case OPUS_INVALID_PACKET:
	  bw = 0;
	  break;
	}
	if(1000*sp->frame_size/SAMPRATE < 5)
	  snprintf(typebuf,sizeof(typebuf),"Opus %.1lf ms",1000.*sp->frame_size/SAMPRATE);
	else
	  snprintf(typebuf,sizeof(typebuf),"Opus %d ms",1000*sp->frame_size/SAMPRATE);	  
	type = typebuf;
	break;
      default:
	snprintf(typebuf,sizeof(typebuf),"%d",sp->type);
	bw = 0; // Unknown
	type = typebuf;
	break;
      }
      wmove(Mainscr,row,1);
      wclrtoeol(Mainscr);

      if(!sp->dest) // Might not be allocated yet, if we got dispatched during the nameinfo() call
	continue;
      if(strlen(sp->src_addr) == 0){
	getnameinfo((struct sockaddr *)&sp->sender,sizeof(sp->sender),sp->src_addr,sizeof(sp->src_addr),
		    //		    sp->src_port,sizeof(sp->src_port),NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
		    sp->src_port,sizeof(sp->src_port),NI_NOFQDN|NI_DGRAM);
      }      
      char temp[strlen(sp->src_addr)+strlen(sp->src_port)+strlen(sp->dest) + 20]; // Allow some room
      snprintf(temp,sizeof(temp),"%s:%s -> %s",sp->src_addr,sp->src_port,sp->dest);
      double queue =  (double)(sp->wptr - Rptr)/SAMPRATE;
      mvwprintw(Mainscr,row,0,"%-12s%2d%3d%+5.0lf%+6.2lf%10x%10.2lf %s",
		type,
		sp->channels,
		bw,
		20*log10(sp->gain),
		sp->pan,
		sp->ssrc,
		queue,
		temp);
      if(sp->packets)
	wprintw(Mainscr," packets %'lu",sp->packets);
      if(sp->rtp_state.dupes)
	wprintw(Mainscr," dupes %'lu",sp->rtp_state.dupes);
      if(sp->rtp_state.drops)
	wprintw(Mainscr," drops %'lu",sp->rtp_state.drops);
      
      if(queue >= 0)
	mvwchgat(Mainscr,row,40,5,A_BOLD,0,NULL);

      if(sp == Current)
	mvwchgat(Mainscr,row,18,10,A_STANDOUT,0,NULL);
      row++;
    }
    row++;
    mvwprintw(Mainscr,row++,0,"\u21e5 select next stream");
    mvwprintw(Mainscr,row++,0,"d delete stream");
    mvwprintw(Mainscr,row++,0,"r reset playout buffer");
    mvwprintw(Mainscr,row++,0,"\u2191 volume +1 dB");
    mvwprintw(Mainscr,row++,0,"\u2193 volume -1 dB");
    mvwprintw(Mainscr,row++,0,"\u2192 stereo position right");
    mvwprintw(Mainscr,row++,0,"\u2190 stereo position left");

    if(Verbose){
      // Measure skew between sampling clock and UNIX real time (hopefully NTP synched)
      struct timeval tv;
      gettimeofday(&tv,NULL);
      double unix_seconds = tv.tv_sec - Start_unix_time.tv_sec + 1e-6*(tv.tv_usec - Start_unix_time.tv_usec);
      double pa_seconds = Pa_GetStreamTime(Pa_Stream) - Start_pa_time;
      mvwprintw(Mainscr,row++,0,"D/A clock error: %lf ppm\n",1e6 * (pa_seconds / unix_seconds - 1));
    }
    move(row,0);
    clrtobot();
    mvwprintw(Mainscr,0,0,"KA9Q Multicast Audio Monitor:");
    for(int i=0;i<Nfds;i++)
      wprintw(Mainscr," %s",Mcast_address_text[i]);
    wprintw(Mainscr,"\n");
    wnoutrefresh(Mainscr);
    doupdate();
    if(!Current){
      usleep(1000*Update_interval); // No getch() to slow us down!
      continue;
    }
    // process commands only if there's something to act on
    int c = getch(); // Pauses here
    switch(c){
    case EOF:
      break;
    case KEY_NPAGE:
    case '\t':
      if(Current->next)
	Current = Current->next;
      else if(Session)
	Current = Session; // Wrap around to top
      break;
    case KEY_BTAB:
    case KEY_PPAGE:
      if(Current->prev)
	Current = Current->prev;
      break;
    case KEY_UP:
      Current->gain *= 1.122018454; // 1 dB
      break;
    case KEY_DOWN:
      Current->gain /= 1.122018454;
      break;
    case KEY_LEFT:
      Current->pan = max(Current->pan - .01,-1.0);
      break;
    case KEY_RIGHT:
      Current->pan = min(Current->pan + .01,+1.0);
      break;
    case 'r':
      // Reset playout queue
      Current->wptr = Rptr;
      // Reset counters
      Current->packets = 0;
      Current->rtp_state.dupes = 0;
      Current->rtp_state.drops = 0;
      break;
    break;
    case 'd':
      {
	if(Current){
	  Current->terminate = 1;
	  pthread_cancel(Current->task);
	  pthread_join(Current->task,NULL);
	  close_session(Current);
	  Current = Session;
	}
      }
      break;
    case '\f':  // Screen repaint (formfeed, aka control-L)
      clearok(curscr,TRUE);
      break;
    case 's':
      Pa_AbortStream(Pa_Stream);      
      Pa_StartStream(Pa_Stream);      
      break;
    default:
      break;
    }
  }
  return NULL;
}

struct session *lookup_session(const struct sockaddr_storage *sender,const uint32_t ssrc){
  struct session *sp;
  pthread_mutex_lock(&Sess_mutex);
  for(sp = Session; sp; sp = sp->next){
    if(sp->ssrc == ssrc && memcmp(&sp->sender,sender,sizeof(*sender)) == 0){
      // Found it
      break;
    }
  }
  pthread_mutex_unlock(&Sess_mutex);
  return sp;
}
// Create a new session, partly initialize
struct session *create_session(struct sockaddr_storage const *sender,uint32_t ssrc){
  struct session *sp;

  if(!(sp = calloc(1,sizeof(*sp))))
    return NULL; // Shouldn't happen on modern machines!
  
  // Initialize entry
  memcpy(&sp->sender,sender,sizeof(*sender));
  sp->ssrc = ssrc;

  pthread_mutex_lock(&Sess_mutex);
  // Put at end of list so monitor list doesn't scroll down
  struct session *last = NULL;
  for(struct session *pp = Session; pp != NULL; pp = pp->next)
    last = pp;

  if(last){
    // List not empty
    sp->prev = last;
    last->next = sp;
  } else
    Session = sp;

  pthread_mutex_unlock(&Sess_mutex);
  return sp;
}

int close_session(struct session *sp){
  if(!sp)
    return -1;
  
  // Remove from linked list
  pthread_mutex_lock(&Sess_mutex);
  if(sp->next)
    sp->next->prev = sp->prev;
  if(sp->prev)
    sp->prev->next = sp->next;
  else
    Session = sp->next;
  pthread_mutex_unlock(&Sess_mutex);  
  free(sp);
  return 0;
}
