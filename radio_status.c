#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>

#include "misc.h"
#include "dsp.h"
#include "radio.h"
#include "filter.h"
#include "multicast.h"
#include "status.h"


uint64_t Commands;

struct state State[256];

// Thread to periodically transmit receiver state
void *send_status(void *arg){
  pthread_setname("status");
  assert(arg != NULL);
  struct demod * const demod = arg;

  memset(State,0,sizeof(State));
  
  for(int count=0;;count++){
    if(demod->output.status_fd <= 0){
      usleep(1);
      continue;
    }
    // emit status packets indefinitely
    unsigned char packet[2048],*bp;
    memset(packet,0,sizeof(packet));
    bp = packet;

    *bp++ = 0; // Response (not a command);

    struct timeval tp;
    gettimeofday(&tp,NULL);
    // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
    long long timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;
    encode_int64(&bp,GPS_TIME,timestamp);
    encode_int64(&bp,COMMANDS,Commands);
    // Source information
    // Who's sending us information
    {
      struct sockaddr_in *sin;
      struct sockaddr_in6 *sin6;
      *bp++ = INPUT_SOURCE_SOCKET;
      switch(demod->input.source_address.ss_family){
      case AF_INET:
	sin = (struct sockaddr_in *)&demod->input.source_address;
	*bp++= 6;
	memcpy(bp,&sin->sin_addr.s_addr,4); // Already in network order
	bp += 4;
	memcpy(bp,&sin->sin_port,2);
	bp += 2;
	break;
      case AF_INET6:
	sin6 = (struct sockaddr_in6 *)&demod->input.source_address;
	*bp++ = 10;
	memcpy(bp,&sin6->sin6_addr,8);
	bp += 8;
	memcpy(bp,&sin6->sin6_port,2);
	bp += 2;
	break;
      default:
	break;
      }
    }
    // Destination address (usually multicast) and port on which we're getting input data
    {
      struct sockaddr_in *sin;
      struct sockaddr_in6 *sin6;
      *bp++ = INPUT_DEST_SOCKET;
      switch(demod->input.dest_address.ss_family){
      case AF_INET:
	sin = (struct sockaddr_in *)&demod->input.dest_address;
	*bp++ = 6;
	memcpy(bp,&sin->sin_addr.s_addr,4); // Already in network order
	bp += 4;
	memcpy(bp,&sin->sin_port,2);
	bp += 2;
	break;
      case AF_INET6:
	sin6 = (struct sockaddr_in6 *)&demod->input.dest_address;
	*bp++ = 10;
	memcpy(bp,&sin6->sin6_addr,8);
	bp += 8;
	memcpy(bp,&sin6->sin6_port,2);
	bp += 2;
	break;
      default:
	break;
      }
    }
    encode_int32(&bp,INPUT_SSRC,demod->input.rtp.ssrc);
    encode_int32(&bp,INPUT_SAMPRATE,demod->sdr.status.samprate);
    // Where we're sending output
    {
      struct sockaddr_in *sin;
      struct sockaddr_in6 *sin6;
      *bp++ = OUTPUT_DEST_SOCKET;
      switch(demod->output.dest_address.ss_family){
      case AF_INET:
	sin = (struct sockaddr_in *)&demod->output.dest_address;
	*bp++ = 6;
	memcpy(bp,&sin->sin_addr.s_addr,4); // Already in network order
	bp += 4;
	memcpy(bp,&sin->sin_port,2);
	bp += 2;
	break;
      case AF_INET6:
	sin6 = (struct sockaddr_in6 *)&demod->output.dest_address;
	*bp++ = 10;
	memcpy(bp,&sin6->sin6_addr,8);
	bp += 8;
	memcpy(bp,&sin6->sin6_port,2);
	bp += 2;
	break;
      default:
	break;
      }
    }
    encode_int32(&bp,OUTPUT_SSRC,demod->output.rtp.ssrc);
    encode_byte(&bp,OUTPUT_TTL,Mcast_ttl);
    encode_int32(&bp,OUTPUT_SAMPRATE,demod->output.samprate);
    encode_int64(&bp,INPUT_PACKETS,demod->input.rtp.packets);
    encode_int64(&bp,INPUT_SAMPLES,demod->input.samples);
    encode_int64(&bp,INPUT_DROPS,demod->input.rtp.drops);
    encode_int64(&bp,INPUT_DUPES,demod->input.rtp.dupes);
    encode_int64(&bp,OUTPUT_PACKETS,demod->output.rtp.packets);

    // Tuning
    encode_double(&bp,RADIO_FREQUENCY,get_freq(demod));
    encode_double(&bp,SECOND_LO_FREQUENCY,get_second_LO(demod));
    encode_double(&bp,SHIFT_FREQUENCY,demod->shift.freq);

    // Front end
    encode_double(&bp,FIRST_LO_FREQUENCY,demod->sdr.status.frequency);
    encode_byte(&bp,LNA_GAIN,demod->sdr.status.lna_gain);
    encode_byte(&bp,MIXER_GAIN,demod->sdr.status.mixer_gain);
    encode_byte(&bp,IF_GAIN,demod->sdr.status.if_gain);


    // Doppler info
    encode_double(&bp,DOPPLER_FREQUENCY,get_doppler(demod));
    encode_double(&bp,DOPPLER_FREQUENCY_RATE,get_doppler_rate(demod));

    // Filtering
    encode_float(&bp,LOW_EDGE,demod->filter.low);
    encode_float(&bp,HIGH_EDGE,demod->filter.high);
    encode_float(&bp,KAISER_BETA,demod->filter.kaiser_beta);
    encode_int32(&bp,FILTER_BLOCKSIZE,demod->filter.L);
    encode_int32(&bp,FILTER_FIR_LENGTH,demod->filter.M);
    if(demod->filter.out)
      encode_float(&bp,NOISE_BANDWIDTH,demod->input.samprate * demod->filter.out->noise_gain);

    // Signals - these ALWAYS change
    encode_float(&bp,IF_POWER,demod->sig.if_power);
    encode_float(&bp,BASEBAND_POWER,demod->sig.bb_power);
    encode_float(&bp,NOISE_DENSITY,demod->sig.n0);

    // Demodulation mode
    encode_string(&bp,RADIO_MODE,demod->mode,strlen(demod->mode));
    enum demod_type demod_type = Demodtab[demod->demod_type].demod_type;
    encode_byte(&bp,DEMOD_MODE,demod_type);
    switch(demod_type){
    case AM_DEMOD:
      encode_float(&bp,DEMOD_GAIN,demod->agc.gain);
      break;
    case FM_DEMOD:
      encode_float(&bp,PEAK_DEVIATION,demod->sig.pdeviation);
      encode_float(&bp,PL_TONE,demod->sig.plfreq);
      encode_float(&bp,FREQ_OFFSET,demod->sig.foffset);
      encode_float(&bp,DEMOD_SNR,demod->sig.snr);
      break;
    case LINEAR_DEMOD:
      encode_float(&bp,DEMOD_GAIN,demod->agc.gain);
      encode_int32(&bp,INDEPENDENT_SIDEBAND,demod->filter.isb);
      if(demod->opt.pll){
	encode_float(&bp,FREQ_OFFSET,demod->sig.foffset);
	encode_float(&bp,PLL_PHASE,demod->sig.cphase);
	encode_float(&bp,DEMOD_SNR,demod->sig.snr);
	encode_byte(&bp,PLL_LOCK,demod->sig.pll_lock);
	encode_byte(&bp,PLL_SQUARE,demod->opt.square);
      }
      break;
    }
    encode_int32(&bp,OUTPUT_CHANNELS,demod->output.channels);
    encode_eol(&bp);

    // Every 10th packet is full state; all others include changes only
    int len = compact_packet(&State[0],packet,(count % 10) == 0);
    send(demod->output.status_fd,packet,len,0);
    usleep(100000);
  }
}


void decode_sdr_status(struct demod *demod,unsigned char *buffer,int length);

void *recv_sdr_status(void *arg){
  struct demod *demod = (struct demod *)arg;

  int nctlrx_fd = setup_mcast(demod->input.dest_address_text,NULL,0,0,2);

  while(1){
    unsigned char buffer[8192];

    memset(buffer,0,sizeof(buffer));
    int len = recv(nctlrx_fd,buffer,sizeof(buffer),0);
    if(len <= 0){
      sleep(1);
      continue;
    }
    // Parse entries
    int cr = buffer[0]; // command-response byte

    if(cr == 1)
      continue; // Ignore commands
    
    decode_sdr_status(demod,buffer+1,len-1);
    pthread_mutex_lock(&demod->sdr.status_mutex);
    pthread_cond_broadcast(&demod->sdr.status_cond);
    pthread_mutex_unlock(&demod->sdr.status_mutex);
  }    
}

void decode_sdr_status(struct demod *demod,unsigned char *buffer,int length){
  unsigned char *cp = buffer;
  double nfreq = NAN;
  int gainchange = 0;

  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // End of list

    unsigned int optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // Invalid length
    switch(type){
    case EOL: // Shouldn't get here since it's checked above
      goto done;
    case RADIO_FREQUENCY:
      nfreq = decode_double(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      demod->input.samprate = demod->sdr.status.samprate = decode_int(cp,optlen);
      demod->filter.decimate = demod->sdr.status.samprate / demod->output.samprate;
      break;
    case GPS_TIME:
      demod->sdr.status.timestamp = decode_int(cp,optlen);
      break;
    case LOW_EDGE:
      demod->sdr.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      demod->sdr.max_IF = decode_float(cp,optlen);
      break;
    case LNA_GAIN:
      demod->sdr.status.lna_gain = decode_int(cp,optlen);
      gainchange++;
      break;
    case MIXER_GAIN:
      demod->sdr.status.mixer_gain = decode_int(cp,optlen);
      gainchange++;
      break;
    case IF_GAIN:
      demod->sdr.status.if_gain = decode_int(cp,optlen);
      gainchange++;
      break;
    case DC_I_OFFSET:
      demod->sdr.DC_i = decode_float(cp,optlen);
      break;
    case DC_Q_OFFSET:
      demod->sdr.DC_q = decode_float(cp,optlen);
      break;
    case IQ_IMBALANCE:
      demod->sdr.imbalance = decode_float(cp,optlen);
      break;
    case IQ_PHASE:
      demod->sdr.sinphi = decode_float(cp,optlen);
      break;
    case CALIBRATE:
      demod->sdr.calibration = decode_double(cp,optlen);
      break;
    default:
      break;
    }
    cp += optlen;
  }
  if(gainchange)
    demod->sdr.gain_factor = powf(10.,-0.05*(demod->sdr.status.lna_gain + demod->sdr.status.if_gain + demod->sdr.status.mixer_gain));
  if(!isnan(nfreq) && demod->sdr.status.frequency != nfreq && demod->sdr.status.samprate != 0){
    // Recalculate LO2
    demod->sdr.status.frequency = nfreq;
    double new_LO2 = -(demod->tune.freq - get_first_LO(demod));
    set_second_LO(demod,new_LO2);
  }
  done:;
}



  
