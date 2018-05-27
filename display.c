// $Id: display.c,v 1.123 2018/05/02 01:27:50 karn Exp karn $
// Thread to display internal state of 'radio' and accept single-letter commands
// Why are user interfaces always the biggest, ugliest and buggiest part of any program?
// Copyright 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>

#include "misc.h"
#include "radio.h"
#include "audio.h"
#include "filter.h"
#include "multicast.h"
#include "bandplan.h"

float Spare; // General purpose knob for experiments

// Touch screen position (Raspberry Pi display only - experimental)
int touch_x,touch_y;

extern int Update_interval;

// Screen location of field modification cursor
int mod_x,mod_y;

// Pop up a temporary window with the contents of a file in the
// library directory (usually /usr/local/share/ka9q-radio/)
// then wait for a single keyboard character to clear it
void popup(const char *filename){
  static const int maxcols = 256;
  char fname[PATH_MAX];
  snprintf(fname,sizeof(fname),"%s/%s",Libdir,filename);
  FILE *fp;
  if((fp = fopen(fname,"r")) == NULL)
    return;
  // Determine size of box
  int rows=0, cols=0;
  char line[maxcols];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    rows++;
    if(strlen(line) > cols)
      cols = strlen(line); // Longest line
  }
  rewind(fp);
  
  // Allow room for box
  WINDOW * const pop = newwin(rows+2,cols+2,0,0);
  box(pop,0,0);
  int row = 1; // Start inside box
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    mvwaddstr(pop,row++,1,line);
  }
  fclose(fp);
  wnoutrefresh(pop);
  doupdate();
  timeout(-1); // blocking read - wait indefinitely
  (void)getch(); // Read and discard one character
  timeout(Update_interval);
  werase(pop);
  wrefresh(pop);
  delwin(pop);
}


// Pop up a dialog box, issue a prompt and get a response
void getentry(char const *prompt,char *response,int len){
  WINDOW *pwin = newwin(5,90,15,0);
  box(pwin,0,0);
  mvwaddstr(pwin,1,1,prompt);
  wrefresh(pwin);
  echo();
  timeout(-1);
  // Manpage for wgetnstr doesn't say whether a terminating
  // null is stashed. Hard to believe it isn't, but this is to be sure
  memset(response,0,len);
  wgetnstr(pwin,response,len);
  chomp(response);
  timeout(Update_interval);
  noecho();
  werase(pwin);
  wrefresh(pwin);
  delwin(pwin);
}

static FILE *Tty;
static SCREEN *Term;

void display_cleanup(void){
  echo();
  nocbreak();
  endwin();
  if(Term)
    delscreen(Term);
  Term = NULL;
  if(Tty)
    fclose(Tty);
  Tty = NULL;
}

static int Frequency_lock;

// Adjust the selected item up or down one step
void adjust_item(struct demod *demod,int direction){
  double tunestep;
  
  tunestep = pow(10., (double)demod->tunestep);

  if(!direction)
    tunestep = - tunestep;

  switch(demod->tuneitem){
  case 0: // Carrier frequency
  case 1: // Center frequency - treat the same
    if(!Frequency_lock) // Ignore if locked
      set_freq(demod,get_freq(demod) + tunestep,NAN);
    break;
  case 2: // First LO
    if(fabs(tunestep) < 1)
      break; // First LO can't make steps < 1  Hz
    
    if(demod->tuner_lock) // Tuner is locked, don't change it
      break;

    // Keep frequency but move LO2, which will move LO1
    double new_lo2 = demod->second_LO + tunestep;
    if(LO2_in_range(demod,new_lo2,0))
      set_freq(demod,get_freq(demod),new_lo2);
    break;
  case 3: // IF
    {
      double new_lo2 = demod->second_LO - tunestep;
      if(LO2_in_range(demod,new_lo2,0)){ // Ignore if out of range
	// Vary RF and LO2 (IF) together to keep LO1 the same
	set_freq(demod,get_freq(demod) + tunestep,new_lo2);
      }
    }
    break;
  case 4: // Filter low edge
    demod->low += tunestep;
    set_filter(demod->filter_out,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);
    break;
  case 5: // Filter high edge
    demod->high += tunestep;
    set_filter(demod->filter_out,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);
    break;
  case 6: // Post-detection audio frequency shift
    set_shift(demod,demod->shift + tunestep);
    break;
  case 7: // Kaiser window beta parameter for filter
    demod->kaiser_beta += tunestep;
    if(demod->kaiser_beta < 0)
      demod->kaiser_beta = 0;
    set_filter(demod->filter_out,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);
    break;
  }
}
// Hooks for knob.c (experimental)
// It seems better to just use the Griffin application to turn knob events into keystrokes or mouse events
void adjust_up(void *arg){
  struct demod *demod = arg;
  adjust_item(demod,1);
}
void adjust_down(void *arg){
  struct demod *demod = arg;
  adjust_item(demod,0);
}
void toggle_lock(void *arg){
  struct demod *demod = arg;
  switch(demod->tuneitem){
  case 0:
  case 1:
    Frequency_lock = !Frequency_lock; // Toggle frequency tuning lock
    break;
  case 2:
    demod->tuner_lock = !demod->tuner_lock;
  }
}



#if 0 // highly incomplete, won't compile yet, will probably go away
double scrape_number(WINDOW *win, int y, int x, double **increment){
  char c;
  int i = 0;
  char buf[100];
  int max_y = 0,max_x = 0;
  int right_end = -1;
  int left_end = -1;
  getmaxyx(win,max_y,max_x);

  // Look for right end
  int multiplier = -1;
  int i;
  for(i=x; i < max_x; i++){
    c = mvwinch(win,y,x) & A_CHARTEXT;
    if(c == ',')
      continue;
    else if(isdigit(c))
      multiplier++;
    else
      break;
  }
  right_end = i-1;

  if(c != '.'){
    // Not found to right, look left
    multiplier = 0;
    for(i=x; i >= 0; i--){
    c = mvwinch(win,y,x) & A_CHARTEXT;
    if(c == ',')
      continue;
    else if(isdigit(c))
      multiplier--;
    else
      break;
    }
  }
  left_end = i;
  if(c != '.'){
    // No decimal point found
  }


  // look left for first element of field
  while(x > 0){
    c = mvwinch(win,y,x-1) & A_CHARTEXT;    
    if(!isdigit(c) && c != ',' && c != '.' && c != '-' && c != '+')
      break;
    x--;
  }


  while(x < max_x && i < sizeof(buf)){
    // To end of line
    c = mvwinch(win,y,x++) & A_CHARTEXT;
    if(isdigit(c) || c == '.' || c == '-' || c == '+')
      buf[i++] = c;
  }
  buf[i] = '\0';
  if(strlen(buf) == 0){
    if(increment)
      *increment = NULL; 
    return NAN;
  }
  return strtod(buf,NULL);
}

void increment(void){
  int c;

  c = mvinch(mod_y,mod_x) & A_CHARTEXT;
  if(!isdigit(c))
    return;

  while(1){
    if(c != '9'){
      c++;
      return;
    }
    c = '0';
    for(x = mod_x - 1; x >= 0; x--){
      c = mvinch(mod_y,x) & A_CHARTEXT;
      if(isdigit(c))
	break;
      if(
    }
    

  }
}
void decrement(void){
}

#endif


// Thread to display receiver state, updated at 10Hz by default
// Uses the ancient ncurses text windowing library
// Also services keyboard, mouse and tuning knob, if present
void *display(void *arg){
  // Drop priority back to normal; the display isn't as critical as the stuff handling signals
  // (Remember Apollo 11's 1201/1202 alarms!)
  setpriority(PRIO_PROCESS,0,0);

  pthread_setname("display");
  assert(arg != NULL);
  struct demod * const demod = arg;
  struct audio * const audio = &Audio; // Eventually make parameter

#ifdef linux
  extern void *knob(void *);
  pthread_t pthread_knob;
  pthread_create(&pthread_knob,NULL,knob,demod);

  extern void *touch(void *);
  pthread_t pthread_touch;
  pthread_create(&pthread_touch,NULL,touch,demod);   // Disable for now
#endif

  atexit(display_cleanup);

#if FUNCTIONKEYS
  slk_init(3);
#endif
  // talk directly to the terminal
  Tty = fopen("/dev/tty","r+");
  Term = newterm(NULL,Tty,Tty);
  set_term(Term);
  keypad(stdscr,TRUE);
  timeout(Update_interval); // update interval when nothing is typed
  cbreak();
  noecho();

  // Set up display subwindows
  int row = 0;
  int col = 0;
  WINDOW * const tuning = newwin(8,35,row,col);    // Frequency information
  col += 35;
  WINDOW * const sig = newwin(8,25,row,col); // Signal information
  col += 25;
  WINDOW * const info = newwin(8,42,row,col);     // Band information
  row += 8;
  col = 0;
  WINDOW * const filtering = newwin(12,22,row,col);
  col += 22;
  WINDOW * const demodulator = newwin(12,25,row,col);
  col += 25;
  WINDOW * const options = newwin(12,12,row,col); // Demod options
  col += 12;
  WINDOW * const sdr = newwin(12,24,row,col); // SDR information
  col += 24;

  WINDOW * const modes = newwin(Nmodes+2,7,row,col);
  col += Nmodes+2;

  col = 0;
  row += 12;
  WINDOW * const network = newwin(8,78,row,col); // Network status information
  col = 0;
  row += 8;
  WINDOW * const debug = newwin(8,78,row,col); // Note: overlaps function keys
  scrollok(debug,1);

  // A message from our sponsor...
  wprintw(debug,"KA9Q SDR Receiver v1.0; Copyright 2017 Phil Karn\n");
  wprintw(debug,"Compiled on %s at %s\n",__DATE__,__TIME__);


  struct sockaddr old_input_source_address;
  char source[NI_MAXHOST];
  char sport[NI_MAXSERV];
  memset(source,0,sizeof(source));
  memset(sport,0,sizeof(sport));

  mmask_t mask = ALL_MOUSE_EVENTS;
  mousemask(mask,NULL);
  MEVENT mouse_event;

  struct timeval last_time;
  gettimeofday(&last_time,NULL);
  long long lastsamples = demod->samples;
  float actual_sample_rate = demod->samprate; // Initialize with nominal

  for(;;){
    // update display indefinitely, handle user commands

    // Tuning control window - these can be adjusted by the user
    // using the keyboard or tuning knob, so be careful with formatting
    wmove(tuning,0,0);
    int row = 1;
    int col = 1;
    if(Frequency_lock)
      wattron(tuning,A_UNDERLINE); // Underscore means the frequency is locked
    mvwprintw(tuning,row,col,"%'28.3f Hz",get_freq(demod)); // RF carrier frequency
    mvwaddstr(tuning,row,col,"Carrier");
    row++;

    // Center of passband
    mvwprintw(tuning,row,col,"%'28.3f Hz",get_freq(demod) + (demod->high + demod->low)/2);
    mvwaddstr(tuning,row++,col,"Center");

    wattroff(tuning,A_UNDERLINE);
    if(demod->tuner_lock)
      wattron(tuning,A_UNDERLINE);    

    // second LO frequency is negative of IF, i.e., a signal at +48 kHz
    // needs a second LO frequency of -48 kHz to bring it to zero
    mvwprintw(tuning,row,col,"%'28.3f Hz",get_first_LO(demod));
    mvwaddstr(tuning,row++,col,"First LO");
    wattroff(tuning,A_UNDERLINE);

#if 0
    // This indication needs to be redone
    if(!LO2_in_range(demod,demod->second_LO,1)){
      // LO2 is near its edges where signals from the opposite edge
      // get aliased; warn about this
      double alias;
      if(demod->second_LO > 0)
	alias = get_first_LO(demod) - demod->second_LO + demod->samprate;
      else
	alias = get_first_LO(demod) - demod->second_LO - demod->samprate;	
      wprintw(tuning," alias %'.3f Hz",alias);
    }
#endif
    mvwprintw(tuning,row,col,"%'28.3f Hz",-demod->second_LO);
    mvwaddstr(tuning,row++,col,"IF");

    // Doppler info displayed only if active
    double dopp = get_doppler(demod);
    if(dopp != 0){
      mvwprintw(tuning,row,col,"%'28.3f Hz",dopp);
      mvwaddstr(tuning,row++,col,"Doppler");
      mvwprintw(tuning,row,col,"%'28.3f Hz/s",get_doppler_rate(demod));
      mvwaddstr(tuning,row++,col,"Dop rate");
    }
    wmove(tuning,row,0);
    wclrtobot(tuning);

    box(tuning,0,0);
    mvwaddstr(tuning,0,15,"Tuning");


    // Display ham band emission data, if available
    // Lines are variable length, so clear window before starting
    wclrtobot(info);  // Output 
    row = 1;
    mvwprintw(info,row++,1,"Receiver profile: %s",demod->mode);

    if(demod->doppler_command)
      mvwprintw(info,row++,1,"Doppler: %s",demod->doppler_command);

    struct bandplan const *bp_low,*bp_high;
    bp_low = lookup_frequency(get_freq(demod)+demod->low);
    bp_high = lookup_frequency(get_freq(demod)+demod->high);
    // Make sure entire receiver passband is in the band
    if(bp_low != NULL && bp_high != NULL){
      struct bandplan r;

      // If the passband straddles a mode or class boundary, form
      // the intersection to give the more restrictive answers
      r.classes = bp_low->classes & bp_high->classes;
      r.modes = bp_low->modes & bp_high->modes;

      mvwprintw(info,row++,1,"Band: %s",bp_low->name);

      if(r.modes){
	mvwaddstr(info,row++,1,"Emissions: ");
	if(r.modes & VOICE)
	  waddstr(info,"Voice ");
	if(r.modes & IMAGE)
	  waddstr(info,"Image ");
	if(r.modes & DATA)
	  waddstr(info,"Data ");
	if(r.modes & CW)
	  waddstr(info,"CW "); // Last since it's permitted almost everywhere
      }
      if(r.classes){
	mvwaddstr(info,row++,1,"Privs: ");
	if(r.classes & EXTRA_CLASS)
	  waddstr(info,"Extra ");
	if(r.classes & ADVANCED_CLASS)
	  waddstr(info,"Adv ");
	if(r.classes & GENERAL_CLASS)
	  waddstr(info,"Gen ");
	if(r.classes & TECHNICIAN_CLASS)
	  waddstr(info,"Tech ");
	if(r.classes & NOVICE_CLASS)
	  waddstr(info,"Nov ");
      }
    }
    box(info,0,0);
    mvwaddstr(info,0,17,"Info");


    int const N = demod->L + demod->M - 1;
    // Filter window values
    row = 1;
    col = 1;
    mvwprintw(filtering,row,col,"%'+17.3f Hz",demod->low);
    mvwaddstr(filtering,row++,col,"Low");
    mvwprintw(filtering,row,col,"%'+17.3f Hz",demod->high);
    mvwaddstr(filtering,row++,col,"High");    
    mvwprintw(filtering,row,col,"%'+17.3f Hz",demod->shift);
    mvwaddstr(filtering,row++,col,"Shift");
    mvwprintw(filtering,row,col,"%'17.3f",demod->kaiser_beta);
    mvwaddstr(filtering,row++,col,"Beta");    
    mvwprintw(filtering,row,col,"%'17d",demod->L);
    mvwaddstr(filtering,row++,col,"Blocksize");
    mvwprintw(filtering,row,col,"%'17d",demod->M);
    mvwaddstr(filtering,row++,col,"FIR");
    mvwprintw(filtering,row,col,"%'17.3f Hz",demod->samprate / N);
    mvwaddstr(filtering,row++,col,"Freq bin");
    mvwprintw(filtering,row,col,"%'17.3f ms",1000*(N - (demod->M - 1)/2)/demod->samprate); // Is this correct?
    mvwaddstr(filtering,row++,col,"Delay");
    mvwprintw(filtering,row,col,"%17d",demod->interpolate);
    mvwaddstr(filtering,row++,col,"Interpolate");
    mvwprintw(filtering,row,col,"%17d",demod->decimate);
    mvwaddstr(filtering,row++,col,"Decimate");

    box(filtering,0,0);
    mvwaddstr(filtering,0,6,"Filtering");


    // Signal data window
    float bw = 0;
    if(demod->filter_out != NULL)
      bw = demod->samprate * demod->filter_out->noise_gain;
    float sn0 = demod->bb_power / demod->n0 - bw;
    sn0 = max(sn0,0.0f); // Can go negative due to inconsistent smoothed values; clip it at zero

    row = 1;
    col = 1;
    mvwprintw(sig,row,col,"%15.1f dBFS",power2dB(demod->if_power));
    mvwaddstr(sig,row++,col,"IF");
    mvwprintw(sig,row,col,"%15.1f dBFS",power2dB(demod->bb_power));
    mvwaddstr(sig,row++,col,"Baseband");
    mvwprintw(sig,row,col,"%15.1f dBFS/Hz",power2dB(demod->n0));
    mvwaddstr(sig,row++,col,"N0");
    mvwprintw(sig,row,col,"%15.1f dBHz",10*log10f(sn0));
    mvwaddstr(sig,row++,col,"S/N0");
    mvwprintw(sig,row,col,"%15.1f dBHz",10*log10f(bw));
    mvwaddstr(sig,row++,col,"NBW");
    mvwprintw(sig,row,col,"%15.1f dB",10*log10f(sn0/bw));
    mvwaddstr(sig,row++,col,"SNR");
    box(sig,0,0);
    mvwaddstr(sig,0,9,"Signal");


    // Demodulator info
    wmove(demodulator,0,0);
    wclrtobot(demodulator);    
    row = 1;
    int rcol = 9;
    int lcol = 1;
    // Display only if used by current mode
    if(demod->snr >= 0){
      mvwprintw(demodulator,row,rcol,"%11.1f dB",power2dB(demod->snr));
      mvwaddstr(demodulator,row++,lcol,"Loop SNR");
    }
    if(demod->gain >= 0){
      mvwprintw(demodulator,row,rcol,"%11.1f dB",voltage2dB(demod->gain));
      mvwaddstr(demodulator,row++,lcol,"AF Gain");
    }    
    if(!isnan(demod->foffset)){
      mvwprintw(demodulator,row,rcol,"%'+11.3f Hz",demod->foffset);
      mvwaddstr(demodulator,row++,lcol,"Offset");
    }
    if(!isnan(demod->pdeviation)){
      mvwprintw(demodulator,row,rcol,"%11.1f Hz",demod->pdeviation);
      mvwaddstr(demodulator,row++,lcol,"Deviation");
    }
    if(!isnan(demod->cphase)){
      mvwprintw(demodulator,row,rcol,"%+11.1f deg",demod->cphase*DEGPRA);
      mvwaddstr(demodulator,row++,lcol,"Phase");
    }
    if(!isnan(demod->plfreq)){
      mvwprintw(demodulator,row,rcol,"%11.1f Hz",demod->plfreq);
      mvwaddstr(demodulator,row++,lcol,"Tone");
    }
    if(!isnan(demod->spare)){
      mvwprintw(demodulator,row,rcol,"%11.1f",demod->spare);      
      mvwaddstr(demodulator,row++,lcol,"Spare");
    }
    box(demodulator,0,0);
    mvwprintw(demodulator,0,5,"%s demodulator",demod->demod_name);


    // SDR hardware status: sample rate, tcxo offset, I/Q offset and imbalance, gain settings
    row = 1;
    col = 1;
    mvwprintw(sdr,row,col,"%'18d Hz",demod->status.samprate); // Nominal
    mvwaddstr(sdr,row++,col,"Samprate");
    mvwprintw(sdr,row,col,"%'18.0f Hz",demod->status.frequency); // Integer for now (SDR dependent)
    mvwaddstr(sdr,row++,col,"LO");
    mvwprintw(sdr,row,col,"%'+18.5f ppm",demod->calibrate *1e6);
    mvwaddstr(sdr,row++,col,"TCXO cal");
    mvwprintw(sdr,row,col,"%+18.6f",demod->DC_i);  // Scaled to +/-1
    mvwaddstr(sdr,row++,col,"I offset");
    mvwprintw(sdr,row,col,"%+18.6f",demod->DC_q);
    mvwaddstr(sdr,row++,col,"Q offset");
    mvwprintw(sdr,row,col,"%+18.3f dB",power2dB(demod->imbalance));
    mvwaddstr(sdr,row++,col,"I/Q imbal");
    mvwprintw(sdr,row,col,"%+18.1f deg",demod->sinphi*DEGPRA);
    mvwaddstr(sdr,row++,col,"I/Q phi");
    mvwprintw(sdr,row,col,"%18u",demod->status.lna_gain);   // SDR dependent
    mvwaddstr(sdr,row++,col,"LNA");
    mvwprintw(sdr,row,col,"%18u",demod->status.mixer_gain); // SDR dependent
    mvwaddstr(sdr,row++,col,"Mix gain");
    mvwprintw(sdr,row,col,"%18u dB",demod->status.if_gain); // SDR dependent    
    mvwaddstr(sdr,row++,col,"IF gain");
    box(sdr,0,0);
    mvwaddstr(sdr,0,6,"SDR Hardware");


    // Demodulator options, can be set with mouse
    row = 1;
    col = 1;
    if(demod->flags & ISB)
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"ISB");
    wattroff(options,A_UNDERLINE);

    if(demod->flags & PLL)
      wattron(options,A_UNDERLINE);      
    mvwprintw(options,row++,col,"PLL");
    wattroff(options,A_UNDERLINE);

    if(demod->flags & CAL)
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"Calibrate");
    wattroff(options,A_UNDERLINE);      

    if(demod->flags & SQUARE)
      wattron(options,A_UNDERLINE);            
    mvwprintw(options,row++,col,"Square");
    wattroff(options,A_UNDERLINE);

    if(demod->flags & MONO)
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"Mono");    
    wattroff(options,A_UNDERLINE);

    if(!(demod->flags & MONO))
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"Stereo");    
    wattroff(options,A_UNDERLINE);
    
    box(options,0,0);
    mvwaddstr(options,0,2,"Options");


    // Display list of modes defined in /usr/local/share/ka9q-radio/modes.txt
    // Underline the active one
    // Can be selected with mouse
    row = 1; col = 1;
    for(int i=0;i<Nmodes;i++){
      if(strcasecmp(demod->mode,Modes[i].name) == 0)
	wattron(modes,A_UNDERLINE);
      mvwaddstr(modes,row++,col,Modes[i].name);
      wattroff(modes,A_UNDERLINE);
    }
    box(modes,0,0);
    mvwaddstr(modes,0,1,"Modes");


    // Network status window
    if(memcmp(&old_input_source_address,&demod->input_source_address,sizeof(old_input_source_address)) != 0){
      // First time, or source has changed
      memcpy(&old_input_source_address,&demod->input_source_address,sizeof(old_input_source_address));
      getnameinfo((struct sockaddr *)&demod->input_source_address,sizeof(demod->input_source_address),
		  source,sizeof(source),
		  sport,sizeof(sport),NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
    }
    row = 1;
    col = 1;
    extern uint32_t Ssrc;

    // Estimate actual I/Q sample rate against local time of day clock
    struct timeval current_time;
    gettimeofday(&current_time,NULL);
    double interval = current_time.tv_sec - last_time.tv_sec
      + (current_time.tv_usec - last_time.tv_usec)/1.e6;
    double instant_sample_rate = (demod->samples - lastsamples) / interval;
    if(instant_sample_rate < 10*actual_sample_rate) // Suppress glitches, especially at startup. '10' is empirically determined
      actual_sample_rate += .002 * (instant_sample_rate - actual_sample_rate); // .002 is empirical

    last_time = current_time;
    lastsamples = demod->samples;

    wmove(network,0,0);
    wclrtoeol(network);
    mvwprintw(network,row++,col,"Source: %s:%s -> %s",source,sport,demod->iq_mcast_address_text);
    mvwprintw(network,row++,col,"IQ pkts %'llu samples %'llu rate %'.3lf Hz",
	      demod->rtp_state.packets,demod->samples,actual_sample_rate);
    if(demod->rtp_state.drops)
      wprintw(network," drops %'llu",demod->rtp_state.drops);
    if(demod->rtp_state.dupes)
      wprintw(network," dupes %'llu",demod->rtp_state.dupes);

    mvwprintw(network,row++,col,"Time: %s",lltime(demod->status.timestamp));
    mvwprintw(network,row++,col,"Sink: %s; ssrc %8x; TTL %d%s",audio->audio_mcast_address_text,
	      Ssrc,Mcast_ttl,Mcast_ttl == 0 ? " (Local host only)":"");
    mvwprintw(network,row++,col,"PCM %'d Hz; pkts %'llu",audio->samprate,audio->audio_packets);

    box(network,0,0);
    mvwaddstr(network,0,35,"I/O");

    touchwin(debug); // since we're not redrawing it every cycle

    // Highlight cursor for tuning step
    // A little messy because of the commas in the frequencies
    // They come from the ' option in the printf formats
    // tunestep is the log10 of the digit position (0 = units)
    int hcol;
    if(demod->tunestep >= 0){
      hcol = demod->tunestep + demod->tunestep/3;
      hcol = -hcol;
    } else {
      hcol = -demod->tunestep;
      hcol = 1 + hcol + (hcol-1)/3; // 1 for the decimal point, and extras if there were commas in more than 3 places
    }
    switch(demod->tuneitem){
    case 0:
    case 1:
    case 2:
    case 3:
      mod_y = demod->tuneitem + 1;
      mod_x = 24 + hcol; // units in column 24
      mvwchgat(tuning,mod_y,mod_x,1,A_STANDOUT,0,NULL);
      break;
    case 4:
    case 5:
    case 6:
    case 7:
      mod_y = demod->tuneitem - 3;
      mod_x = 13 + hcol; // units in column 13
      mvwchgat(filtering,mod_y,mod_x,1,A_STANDOUT,0,NULL);
      break;
    default:
      ;
      break;
    }
    wnoutrefresh(tuning);
    wnoutrefresh(debug);
    wnoutrefresh(info);
    wnoutrefresh(filtering);
    wnoutrefresh(sig);
    wnoutrefresh(demodulator);
    wnoutrefresh(sdr);
    wnoutrefresh(options);
    wnoutrefresh(modes);
    wnoutrefresh(network);
#if FUNCTIONKEYS
    // Write function key labels for current mode
    // These didn't turn out to be very useful
    // There aren't enough function keys to go around and
    // just using the mouse or entering the textual mode name seems easier
    slk_set(1,"FM",1);
    slk_set(2,"AM",1);
    slk_set(3,"USB",1);
    slk_set(4,"LSB",1);
    slk_set(5,"CW",1);    
    slk_set(6,"PLL",1);
    slk_set(7,"CAL",1);
    slk_set(8,"SQR",1);
    slk_set(9,"ISB",1);
    slk_set(11,"STEREO",1);
    slk_set(12,"MONO",1);

    slk_noutrefresh(); // Do this after debug refresh since debug can overlap us
#endif
    doupdate();      // Update the screen right before we pause
    
    // Scan and process keyboard commands
    int c = getch(); // read keyboard with timeout; controls refresh rate

    switch(c){
#if FUNCTIONKEYS
    case KEY_F(1):
      set_mode(demod,"FM",1);
      break;
    case KEY_F(2):
      set_mode(demod,"AM",1);
      break;
    case KEY_F(3):
      set_mode(demod,"USB",1);
      break;
    case KEY_F(4):
      set_mode(demod,"LSB",1);
      break;
    case KEY_F(5):
      set_mode(demod,"CWU",1);
      break;
    case KEY_F(6):
      demod->flags ^= PLL;
      break;
    case KEY_F(7):
      demod->flags ^= CAL;
      if(demod->flags & CAL)
	demod->flags |= PLL;
      break;
    case KEY_F(8):
      demod->flags ^= SQUARE;
      if(demod->flags & SQUARE)
	demod->flags |= PLL;
      break;
    case KEY_F(9):
      demod->flags ^= ISB;
      break;
    case KEY_F(10):
      break;
    case KEY_F(11):
      demod->flags &= ~MONO;
      break;
    case KEY_F(12):
      demod->flags |= MONO;
      break;
#endif
    case KEY_MOUSE: // Mouse event
      getmouse(&mouse_event);
      break;
    case ERR:   // no key; timed out. Do nothing.
      break;
    case 'q':   // Exit entire radio program. Should this be removed? ^C also works.
      goto done;
    case 'h':
    case '?':
      popup("help.txt");
      break;
    case 'w': // Save radio state to file in ~/.radiostate
      {
	char str[160];
	getentry("Save state file: ",str,sizeof(str));
	if(strlen(str) > 0)
	  savestate(demod,str);
      }
      break;
    case 'I': // Change multicast address for input I/Q stream
      {
	char str[160];
	getentry("IQ input IP dest address: ",str,sizeof(str));
	if(strlen(str) <= 0)
	  break;

	int const i = setup_mcast(str,0);
	if(i == -1){
	  beep();
	  break;
	}
	// demod->input_fd is not protected by a mutex, so swap it carefully
	// Mutex protection would be difficult because input thread is usually
	// blocked on the socket, and if there's no I/Q input we'd hang
	int const j = demod->input_fd;
	demod->input_fd = i;
	if(j != -1)
	  close(j); // This should cause the input thread to see an error
	strlcpy(demod->iq_mcast_address_text,str,sizeof(demod->iq_mcast_address_text));
	// Clear RTP receiver state
	memset(&demod->rtp_state,0,sizeof(demod->rtp_state));
      }
      break;
    case 'l': // Toggle RF or first LO lock; affects how adjustments to LO and IF behave
      toggle_lock(demod);
      break;
    case KEY_NPAGE: // Page Down/tab key
    case '\t':      // go to next tuning item
      demod->tuneitem = (demod->tuneitem + 1) % 8;
      break;
    case KEY_BTAB:  // Page Up/Backtab, i.e., shifted tab:
    case KEY_PPAGE: // go to previous tuning item
      demod->tuneitem = (8 + demod->tuneitem - 1) % 8;
      break;
    case KEY_HOME: // Go back to item 0
      demod->tuneitem = 0;
      demod->tunestep = 0;
      break;
    case KEY_BACKSPACE: // Cursor left: increase tuning step 10x
    case KEY_LEFT:
      if(demod->tunestep >= 9){
	beep();
	break;
      }
      demod->tunestep++;
      break;
    case KEY_RIGHT:     // Cursor right: decrease tuning step /10
      if(demod->tunestep <= -3){
	beep();
	break;
      }
      demod->tunestep--;
      break;
    case KEY_UP:        // Increase whatever digit we're tuning
      adjust_up(demod);
      break;
    case KEY_DOWN:      // Decrease whatever we're tuning
      adjust_down(demod);
      break;
    case '\f':  // Screen repaint (formfeed, aka control-L)
      clearok(curscr,TRUE);
      break;
    case 'b':   // Blocksize - sets both data and impulse response-1
                // They should be separably set. Do this in the state file for now
      {
	char str[160],*ptr;
	getentry("Enter blocksize in samples: ",str,sizeof(str));
	int const i = strtol(str,&ptr,0);
	if(ptr == str)
	  break; // Nothing entered
	
	demod->L = i;
	demod->M = demod->L + 1;
	set_mode(demod,demod->mode,0); // Restart demod thread
      }
      break;
    case 'c':   // TCXO calibration offset, also affects sampling clock
      {
	char str[160],*ptr;
	getentry("Enter calibration offset in ppm: ",str,sizeof(str));
	double const f = strtod(str,&ptr);
	if(ptr == str)
	  break;
	set_cal(demod,f * 1e-6);
      }
      break;
    case 'm': // Manually set modulation mode
      {
	char str[1024];
	snprintf(str,sizeof(str),"Enter mode [ ");
	for(int i=0;i < Nmodes;i++){
	  strlcat(str,Modes[i].name,sizeof(str) - strlen(str) - 1);
	  strlcat(str," ",sizeof(str) - strlen(str) - 1);
	}
	strlcat(str,"]: ",sizeof(str) - strlen(str) - 1);
	getentry(str,str,sizeof(str));
	if(strlen(str) <= 0)
	  break;
	set_mode(demod,str,1);
      }
      break;
    case 'f':   // Tune to new radio frequency
      {
	char str[160];
	getentry("Enter carrier frequency: ",str,sizeof(str));
	double const f = parse_frequency(str); // Handles funky forms like 147m435
	if(f <= 0)
	  break; // Invalid

	// If frequency would be out of range, guess kHz or MHz
	if(f >= 0.1 && f < 100)
	  set_freq(demod,f*1e6,NAN); // 0.1 - 99.999 Only MHz can be valid
	else if(f < 500)         // 100-499.999 could be kHz or MHz, assume MHz
	  set_freq(demod,f*1e6,NAN);
	else if(f < 2000)        // 500-1999.999 could be kHz or MHz, assume kHz
	  set_freq(demod,f*1e3,NAN);
	else if(f < 100000)      // 2000-99999.999 can only be kHz
	  set_freq(demod,f*1e3,NAN);
	else                     // accept directly
	  set_freq(demod,f,NAN);
      }
      break;
    case 'i':    // Recenter IF to +/- samprate/4
      set_freq(demod,get_freq(demod),demod->samprate/4);
      break;
    case 'u':    // Set display update rate in milliseconds (minimum 50, i.e, 20 Hz)
      {
	char str[160],*ptr;
	getentry("Enter update interval, ms [<=0 means no auto update]: ",str,sizeof(str));
	int const u = strtol(str,&ptr,0);
	if(ptr == str)
	  break; // Nothing entered
	
	if(u > 50){
	  Update_interval = u;
	  timeout(Update_interval);
	} else if(u <= 0){
	  Update_interval = -1; // No automatic update
	  timeout(Update_interval);
	} else
	  beep();
      }
      break;
    case 'k': // Kaiser window beta parameter
      {
	char str[160],*ptr;
	getentry("Enter Kaiser window beta: ",str,sizeof(str));
	double const b = strtod(str,&ptr);
	if(ptr == str)
	  break; // nothing entered
	if(b < 0 || b >= 100){
	  beep();
	  break; // beyond limits
	}
	if(b != demod->kaiser_beta){
	  demod->kaiser_beta = b;
	  set_filter(demod->filter_out,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);
	}
      }
      break;
    case 'o': // Set/clear option flags, most apply only to linear detector
      {
	char str[160];
	getentry("Enter option [isb pll cal flat square stereo mono], '!' prefix disables: ",str,sizeof(str));
	if(strcasecmp(str,"mono") == 0){
	  demod->flags |= MONO;
	} else if(strcasecmp(str,"!mono") == 0){
	  demod->flags &= ~MONO;
	} else if(strcasecmp(str,"stereo") == 0){
	  demod->flags &= ~MONO;	  
	} else if(strcasecmp(str,"isb") == 0){
	  demod->flags |= ISB;
	} else if(strcasecmp(str,"!isb") == 0){
	  demod->flags &= ~ISB;
	} else if(strcasecmp(str,"pll") == 0){
	  demod->flags |= PLL;
	} else if(strcasecmp(str,"!pll") == 0){
	  demod->flags &= ~(PLL|SQUARE|CAL);
	} else if(strcasecmp(str,"square") == 0){
	  demod->flags |= SQUARE|PLL;
	} else if(strcasecmp(str,"!square") == 0){	  
	  demod->flags &= ~SQUARE;
	} else if(strcasecmp(str,"cal") == 0){
	  demod->flags |= CAL|PLL;
	} else if(strcasecmp(str,"!cal") == 0){
	  demod->flags &= ~CAL;
	} else if(strcasecmp(str,"flat") == 0){
	  demod->flags |= FLAT;
	} else if(strcasecmp(str,"!flat") == 0){
	  demod->flags &= ~FLAT;
	}
      }
      break;
    default:
      beep();
      break;
    }
    // Process mouse events
    // Need to handle the wheel as equivalent to up/down arrows
    int mx,my;
    mx = mouse_event.x;
    my = mouse_event.y;
    mouse_event.y = mouse_event.x = mouse_event.z = 0;
    if(mx != 0 && my != 0){
#if 0
      wprintw(debug," (%d %d)",mx,my);
#endif
      if(wmouse_trafo(tuning,&my,&mx,false)){
	// Tuning window
	demod->tuneitem = my-1;
	demod->tunestep = 24-mx;
	if(demod->tunestep < 0)
	  demod->tunestep++;
	if(demod->tunestep > 3)
	  demod->tunestep--;
	if(demod->tunestep > 6)
	  demod->tunestep--;
	if(demod->tunestep > 9)	
	  demod->tunestep--;
	// Clamp to range
	if(demod->tunestep < -3)
	  demod->tunestep = -3;
	if(demod->tunestep > 9)
	  demod->tunestep = 9;

      } else if(wmouse_trafo(filtering,&my,&mx,false)){
	// Filter window
	demod->tuneitem = my + 3;
	demod->tunestep = 13-mx;
	if(demod->tunestep < 0)
	  demod->tunestep++;
	if(demod->tunestep > 3)
	  demod->tunestep--;
	if(demod->tunestep > 6)
	  demod->tunestep--;
	if(demod->tunestep > 9)	
	  demod->tunestep--;
	// Clamp to range
	if(demod->tunestep < -3)
	  demod->tunestep = -3;
	if(demod->tunestep > 5)
	  demod->tunestep = 5;
      } else if(wmouse_trafo(modes,&my,&mx,false)){
	// In the options window?
	my--;
	if(my >= 0 && my < Nmodes){
	  set_mode(demod,Modes[my].name,1);
	}
      } else if(wmouse_trafo(options,&my,&mx,false)){
	// In the modes window
	switch(my){
	case 1:
	  demod->flags ^= ISB;
	  break;
	case 2:
	  demod->flags ^= PLL;
	  break;
	case 3:
	  demod->flags ^= CAL;
	  if(demod->flags & CAL)
	    demod->flags |= PLL;
	  break;
	case 4:
	  demod->flags ^= SQUARE;
	  if(demod->flags & SQUARE)
	    demod->flags |= PLL;
	  break;
	case 5:
	  demod->flags |= MONO;
	  break;
	case 6:
	  demod->flags &= ~MONO;
	  break;
	}
      }
    }
  }
 done:;
  endwin();
  set_term(NULL);
  if(Term != NULL)
    delscreen(Term);
  //  if(Tty != NULL)
  //    fclose(Tty);
  
  // Dump receiver state to default
  savestate(demod,"default");
  audio_cleanup(&Audio);
  exit(0);
}


// character size 16 pix high x 9 wide??
void touchitem(void *arg,int x,int y,int ev){
  touch_x = x /8;
  touch_y = y / 16;
}

