// $Id: radio.h,v 1.65 2018/04/22 18:05:02 karn Exp $
// Internal structures and functions of the 'radio' program
// Nearly all internal state is in the 'demod' structure
// More than one can exist in the same program,
// but so far it seems easier to just run separate instances of the 'radio' program.
// Copyright 2018, Phil Karn, KA9Q
#ifndef _RADIO_H
#define _RADIO_H 1

#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <complex.h>
#undef I

#include <sys/types.h>
#include <sys/socket.h>

#include "sdr.h"
#include "multicast.h"

// Internal format of entries in /usr/local/share/ka9q-radio/modes.txt

struct modetab {
  char name[16];
  char demod_name[16];
  void * (*demod)(void *); // Address of demodulator routine
  int flags;        // Special purpose flags, e.g., ISB
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
  // Global parameters

  // Input thread state
  pthread_t rtp_recv_thread;
  int input_fd;      // Raw incoming I/Q data from multicast socket
  char iq_mcast_address_text[256];
  struct sockaddr input_source_address;
  struct sockaddr ctl_address;
  long long iq_packets; // Count of I/Q input packets received
  long long samples;    // Count of raw I/Q samples received

  struct status requested_status; // The status we want the FCD to be in
  struct status status;           // Last status from FCD
  int tuner_lock;                 // When set, don't try to command tuner
  // 'status' is written by the input thread and read by set_first_LO, etc, so it's protected by a mutex
  pthread_mutex_t status_mutex;
  pthread_cond_t status_cond;     // Signalled whenever status changes

  // True A/D sample rate, assuming same TCXO as tuner
  // Set from I/Q packet header and calibrate parameter
  volatile double samprate;

  // queue of RTP packets between rtp-recv and procsamp
  pthread_cond_t qcond;
  pthread_mutex_t qmutex;
  struct packet *queue;

  struct rtp_state rtp_state; // State of the I/Q RTP receiver

  // Processes sequenced RTP packets from the RTP receiver thread
  // Apply I/Q corrections, spin down with 2nd (software) local oscillator,
  // apply Doppler corrections (if used), and pass to input half of pre-detection filter
  pthread_t proc_samples;

  // I/Q correction parameters
  float DC_i,DC_q;       // Average DC offsets
  float sinphi;          // smoothed estimate of I/Q phase error
  float imbalance;       // Ratio of I power to Q power

  double freq;              // Desired carrier frequency

  // Tuning parameters
  int ctl_fd;                     // File descriptor for controlling SDR frequency and gaim

  // The tuner and A/D converter are clocked from the same TCXO
  // Ratio is (1+calibrate)
  //     calibrate < 0 --> TCXO frequency low; calibrate > 0 --> TCXO frequency high
  // True first LO = (1 + calibrate) * demod.status.frequency
  // True A/D sample rate = (1 + calibrate) * demod.status.samprate
  double calibrate;

  // Limits on usable IF due to aliasing, filtering, etc
  // Less than or equal to +/- samprate/2
  float min_IF;
  float max_IF;


  // Doppler shift correction (optional)
  pthread_t doppler_thread;          // Thread that reads file and sets doppler
  char *doppler_command;             // Command to execute for tracking

  pthread_mutex_t doppler_mutex;     // Protects doppler
  double doppler;       // Open-loop doppler correction from satellite tracking program
  double doppler_rate;
  complex double doppler_phasor;
  complex double doppler_phasor_step;
  complex double doppler_phasor_step_step;  

  // Second LO parameters
  pthread_mutex_t second_LO_mutex;
  complex double second_LO_phasor; // Second LO phasor
  double second_LO;     // True second LO frequency, including calibration
                        // Provided because round trip through csincos/carg is less accurate
  complex double second_LO_phasor_step;  // LO step phasor = csincos(2*pi*second_LO/samprate)

  pthread_mutex_t shift_mutex; // protects passband shift
  double shift;         // frequency shift after demodulation (for CW,DSB)
  complex double shift_phasor;
  complex double shift_phasor_step;

  int tunestep;       // Tuning column, log10(); e.g., 3 -> thousands
  int tuneitem;       // Tuning entry index

  // Experimental notch filter
  struct notchfilter *nf;

  // Zero IF pre-demod filter params
  struct filter_in *filter_in;
  struct filter_out *filter_out;
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

  // Mode-specific demodulator thread
  // Run output half of pre-detection filter and pass through AM, FM or linear demodulator
  // The AM and linear demodulators send baseband audio directly to the network;
  // the FM demodulator performs further audio filtering
  pthread_t demod_thread;
  void * (*demod)(void *);        // Entry point to demodulator
  char mode[16];                  // printable mode name
  char demod_name[16];
  int terminate;                  // set to 1 by set_mode() to request graceful termination
  int flags;                      // Special flags to demodulator
// Modetab flags
#define ISB 1      // Cross-conjugation of positive and negative frequencies, for ISB
#define FLAT 2      // No baseband filtering for FM
#define PLL 4  // Coherent carrier tracking
#define CAL 8       // Calibrate mode in coherent demod; adjust calibrate rather than frequency
#define SQUARE   16 // Square carrier in coherent loop (BPSK/suppressed carrier AM)
#define ENVELOPE 32 // Envelope detection of AM
#define MONO     64 // Only output I channel of linear mode

  // Demodulator configuration settngs
  float headroom;   // Audio level headroom
  float hangtime;   // Linear AGC hang time, seconds
  float recovery_rate; // Linear AGC recovery rate, dB/sec (must be positive)
  float attack_rate;   // Linear AGC attack rate, dB/sec (must be negative)
  float loop_bw;    // Loop bw (coherent modes)

  // Demodulator status variables
  float if_power;   // Average power of signal before filter
  float bb_power;   // Average power of signal after filter
  float n0;         // Noise spectral density esimate (experimemtal)
  float snr;        // Estimated signal-to-noise ratio (only some demodulators)
  float gain;       // Audio gain
  float foffset;    // Frequency offset (FM, coherent AM, cal, dsb)
  float pdeviation; // Peak frequency deviation (FM)
  float cphase;     // Carrier phase change (DSB/PSK)
  float plfreq;     // PL tone frequency (FM);
  float spare;      // Currently used for PLL lock hysteresis

  struct filter_in *audio_master; // FM only
};
extern char Libdir[];
extern int Tunestep;
extern struct modetab Modes[];
extern int Nmodes;


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
void update_status(struct demod *,struct status *);
void *proc_samples(void *);
const float compute_n0(struct demod const *);

// Load mode definition table
int readmodes(char *);

// Save and load (most) receiver state
int savestate(struct demod *,char const *);
int loadstate(struct demod *,char const *);

// Save and load calibration
int loadcal(struct demod *);
int savecal(struct demod *);

// Thread entry points
void *display(void *);
void *keyboard(void *);
void *doppler(void *);

// Demodulator thread entry points
void *demod_fm(void *);
void *demod_am(void *);
void *demod_linear(void *);

#endif
