// $Id: status.c,v 1.9 2018/12/04 04:33:18 karn Exp $
// Thread to emit receiver status packets
// Copyright 2018 Phil Karn, KA9Q

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

// Encode 64-bit integer, byte swapped, leading zeroes suppressed
int encode_int64(unsigned char **buf,enum status_type type,uint64_t x){
  unsigned char *cp = *buf;

  *cp++ = type;

  int len = sizeof(x);
  while(len > 0 && (x & 0xff00000000000000LL) == 0){
    x <<= 8;
    len--;
  }
  *cp++ = len;

  for(int i=0; i<len; i++){
    *cp++ = x >> 56;
    x <<= 8;
  }

  *buf = cp;
  return 2+len;
}


// Single null type byte means end of list
int encode_eol(unsigned char **buf){
  unsigned char *bp = *buf;

  *bp++ = EOL;
  *buf = bp;
  return 1;
}

int encode_byte(unsigned char **buf,enum status_type type,unsigned char x){
  unsigned char *cp = *buf;
  *cp++ = type;
  *cp++ = sizeof(x);
  *cp++ = x;
  *buf = cp;
  return 2+sizeof(x);
}

int encode_int16(unsigned char **buf,enum status_type type,uint16_t x){
  return encode_int64(buf,type,(uint64_t)x);
}

int encode_int32(unsigned char **buf,enum status_type type,uint32_t x){
  return encode_int64(buf,type,(uint64_t)x);
}

int encode_int(unsigned char **buf,enum status_type type,int x){
  return encode_int64(buf,type,(int)x);
}


int encode_float(unsigned char **buf,enum status_type type,float x){
  uint32_t data;

  memcpy(&data,&x,sizeof(data));
  return encode_int32(buf,type,(uint64_t)data);
}

int encode_double(unsigned char **buf,enum status_type type,double x){
  uint64_t data;
  memcpy(&data,&x,sizeof(data));
  return encode_int64(buf,type,data);
}

// Encode byte string without byte swapping
int encode_string(unsigned char **bp,enum status_type type,void *buf,int buflen){
  unsigned char *cp = *bp;
  *cp++ = type;
  if(buflen > 255)
    buflen = 255;
  *cp++ = buflen;
  memcpy(cp,buf,buflen);
  *bp = cp + buflen;
  return 2+buflen;
}


// Decode byte string without byte swapping
void *decode_string(unsigned char **bp,void *buf,int buflen){
  unsigned char *cp = *bp;
  int len = *cp++;
  memcpy(buf,cp,min(len,buflen));
  *bp = cp + len;
  return buf;
}


// Decode encoded variable-length UNSIGNED integers
// At entry, *bp -> length field (not type!)
// Works for byte, short, long, long long
uint64_t decode_int(unsigned char *cp,int len){
  uint64_t result = 0;
  // cp now points to beginning of abbreviated int
  // Byte swap as we accumulate
  while(len-- > 0)
    result = (result << 8) | *cp++;

  return result;
}

float decode_float(unsigned char *cp,int len){
  if(len == 8)
    return (float)decode_double(cp,len);

  uint32_t result = decode_int(cp,len);
  return *(float *)&result;
}

double decode_double(unsigned char *cp,int len){
  if(len == 4)
    return (double)decode_float(cp,len);

  uint64_t result = decode_int(cp,len);
  return *(double *)&result;
}

int compact_packet(struct state *s,unsigned char *pkt,int force){
  unsigned char *input = pkt;
  unsigned char *output = pkt;

  *output++ = *input++; // command/response byte (don't really have to copy)

  // Read new packet into table, copying elements that have changed to output
  while(1){
    int type = *input++;
    if(type == EOL)
      break;
    int len = *input++;
    assert(type >= 0 && type < 256);
    assert(len >= 0 && len < 256);    
    if(force || s[type].length != len || memcmp(s[type].value,input,len) != 0){
      s[type].length = len;
      memcpy(s[type].value,input,len);
      *output++ = type;
      *output++ = len;
      assert(output <= input);
      memmove(output,input,len);
      output += len;
    }
    input += len;
  }
  *output++ = EOL;
  return output - pkt;
}
