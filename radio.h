// $Id: radio.h,v 1.81 2018/12/05 07:08:16 karn Exp $
// Internal structures and functions of the 'radio' program
// Nearly all internal state is in the 'demod' structure
// More than one can exist in the same program,
// but so far it seems easier to just run separate instances of the 'radio' program.
// Copyright 2018, Phil Karn, KA9Q
#ifndef _RADIO_H
#define _RADIO_H 1

#include <pthread.h>
#include <complex.h>
#undef I

#include <sys/socket.h>

#include "sdr.h"
#include "multicast.h"
#include "osc.h"

enum demod_type {
  LINEAR_DEMOD = 0,     // Linear demodulation, i.e., everything else: SSB, CW, DSB, CAM, IQ
  AM_DEMOD,             // AM envelope demodulation
  FM_DEMOD,             // Frequency demodulation
};

struct demodtab {
  enum demod_type demod_type;
  char name[16];
  void *(*demod)(void *); // Address of demodulator routine
};
extern struct demodtab Demodtab[];
extern int Ndemod;

// Internal format of entries in /usr/local/share/ka9q-radio/modes.txt
struct modetab {
  char name[16];
  enum demod_type demod_type;
  int pll;
  int square;
  int channels;     // 1 or 2
  int isb;
  int flat;
  float shift;      // Audio frequency shift (mainly for CW/RTTY)
  float tunestep;   // Default tuning step
  float low;        // Lower edge of IF passband
  float high;       // Upper edge of IF passband
  float attack_rate;
  float recovery_rate;
  float hangtime;
};

#define PKTSIZE 16384
// Incoming RTP packets
// This should probably be extracted into a more general RTP library
struct packet {
  struct packet *next;
  struct rtp_header rtp;
  unsigned char *data;
  int len;
  unsigned char content[PKTSIZE];
};

// Demodulator state block
struct demod {
  struct {
    int fd;       // Socket for raw incoming I/Q data
    int ctl_fd;   // Socket for commands to front end

    char dest_address_text[256];
    struct sockaddr_storage source_address; // Source of I/Q data
    struct sockaddr_storage dest_address;   // Dest of I/Q data (typically multicast)
    struct rtp_state rtp; // State of the I/Q RTP receiver
    long long samples;    // Count of raw I/Q samples received
    int samprate;
    // queue of RTP packets between rtp-recv and procsamp
    pthread_cond_t qcond;
    pthread_mutex_t qmutex;
    struct packet *queue;
  } input;

  // Front end hardware information
  struct {
    struct status status;           // Last status from FCD
    double calibration;
    // I/Q correction parameters
    float DC_i,DC_q;       // Average DC offsets
    float sinphi;          // smoothed estimate of I/Q phase error
    float imbalance;       // Ratio of I power to Q power
    
    // Limits on usable IF due to aliasing, filtering, etc
    // Less than or equal to +/- samprate/2
    float min_IF;
    float max_IF;
    
    float gain_factor;     // Multiply by incoming samples to scale by analog AGC settings
    // 'status' is written by the input thread and read by set_first_LO, etc, so it's protected by a mutex
    pthread_mutex_t status_mutex;
    pthread_cond_t status_cond;     // Signalled whenever status changes
  } sdr;
  
  // Tuning parameters
  struct {
    int lock;       // When set, don't try to command tuner
    double freq;    // Desired carrier frequency
    double shift;   // Post-demod frequency shift
    int step;       // Tuning column, log10(); e.g., 3 -> thousands
    int item;       // Tuning entry index
  } tune;

  pthread_t doppler_thread;          // Thread that reads file and sets doppler (optional)
  char *doppler_command;             // Command to execute for tracking
  struct osc doppler;
  struct osc second_LO;
  struct osc shift;

  // Experimental notch filter
  struct notchfilter *nf;

  // Zero IF pre-demod filter params
  struct {
    struct filter_in *in;
    struct filter_out *out;
    int L;            // Signal samples in FFT buffer
    int M;            // Samples in filter impulse response
    int interpolate;  // Input sample ratio multiplier, should be power of 2
    int decimate;     // output sample rate divisor, should be power of 2
    float low;        // Edges of filter band
    float high;
    // Window shape factor for Kaiser window
    // Transition region is approx sqrt(1+Beta^2)
    // 0 => rectangular window; increasing values widens main lobe and decreases ripple
    float kaiser_beta;
    float noise_bandwidth; // noise bandwidth relative to sample rate
    int isb;     // Independent sideband mode
  } filter;

  // Mode-specific demodulator thread
  // Run output half of pre-detection filter and pass through AM, FM or linear demodulator
  // The AM and linear demodulators send baseband audio directly to the network;
  // the FM demodulator performs further audio filtering
  pthread_t demod_thread;
  int terminate;              // set to 1 by set_mode() to request graceful termination

  enum demod_type demod_type;            // Index into demodulator table (AM, FM, Linear)
  char mode[16];              // printable mode name (USB, LSB, etc)

  struct {
    int flat;    // Flat FM frequency response
    int pll;     // Linear mode PLL tracking of carrier
    int square;  // Squarer on PLL input
    float loop_bw;    // Loop bw (coherent modes)
  } opt;

  // AGC (AM and linear modes)
  struct {
    float headroom;   // Audio level headroom
    float hangtime;   // Linear AGC hang time, seconds
    float recovery_rate; // Linear AGC recovery rate, dB/sec (must be positive)
    float attack_rate;   // Linear AGC attack rate, dB/sec (must be negative)
    float gain;       // Audio gain
  } agc;

  // Signal levels & status
  struct {
    float if_power;   // Input level, unity == 0dBFS
    float bb_power;   // Average power of signal after filter
    float n0;         // Noise spectral density esimate (experimemtal)
    float snr;        // Estimated signal-to-noise ratio (only some demodulators)
    float foffset;    // Frequency offset (FM, coherent AM, dsb)
    float pdeviation; // Peak frequency deviation (FM)
    float cphase;     // Carrier phase change (DSB/PSK)
    float plfreq;     // PL tone frequency (FM);
    float lock_timer; // PLL lock timer
    int pll_lock;
  } sig;
  
  struct filter_in *audio_master; // FM only

  // Output
  struct {
    int samprate;       // Audio D/A sample rate (usually 48 kHz)
    // RTP network streaming
    int silent; // last packet was suppressed (used to generate RTP mark bit)
    struct rtp_state rtp;
    char dest_address_text[256];
    struct sockaddr_storage source_address;
    struct sockaddr_storage dest_address;
    int fd;         // File descriptor for multicast output
    int rtcp_fd;    // File descriptor for RTP control protocol
    int status_fd;  // File descriptor for receiver status
    int channels;   // 1 = mono, 2 = stereo
  } output;
};
extern char Libdir[];
extern int Tunestep;
extern struct modetab Modes[];
extern int Nmodes;
extern int Verbose;
extern int SDR_correct;

// Functions/methods to control a demod instance
void *filtert(void *arg);
int LO2_in_range(struct demod *,double f,int);
double get_freq(struct demod *);
double set_freq(struct demod *,double,double);
double get_shift(struct demod *);
double set_shift(struct demod *,double);
const double get_first_LO(struct demod const *);
double set_first_LO(struct demod *,double);
double get_second_LO(struct demod *);
double set_second_LO(struct demod *,double);
double get_doppler(struct demod *);
double get_doppler_rate(struct demod *);
int set_doppler(struct demod *,double,double);
int set_mode(struct demod *,const char *,int);
int set_cal(struct demod *,double);
void *proc_samples(void *);
const float compute_n0(struct demod const *);

// Load mode definition table
int readmodes(char *);

// Save and load (most) receiver state
int savestate(struct demod *,char const *);
int loadstate(struct demod *,char const *);

// Thread entry points
void *display(void *);
void *keyboard(void *);
void *doppler(void *);
void *status(void *);


// Demodulator thread entry points
void *demod_fm(void *);
void *demod_am(void *);
void *demod_linear(void *);

int send_mono_output(struct demod *,const float *,int);
int send_stereo_output(struct demod *,const float *,int);
int setup_output(struct demod *,int);
void output_cleanup(void *);

extern int Mcast_ttl;

#endif
