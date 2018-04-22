#define _GNU_SOURCE 1
#include <assert.h>
//#include <pthread.h>
#include <stdio.h>
//#include <stdlib.h>
//#include <unistd.h>
#include <string.h>
//#include <errno.h>
#include <ctype.h>
#include <math.h>

#include "ax25.h"


char *get_callsign(char *result,unsigned char *in){
  char callsign[7],c;
  
  memset(callsign,0,sizeof(callsign));
  for(int i=0;i<6;i++){
    c = in[i] >> 1;
    if(c == ' ')
      break;
    callsign[i] = c;
  }
  int ssid = (in[6] >> 1) & 0xf;
  snprintf(result,10,"%s-%d",callsign,ssid);
  return result;
}


int dump_frame(unsigned char *frame,int bytes){

  // By default, no digipeaters; will update if there are any
  unsigned char *control = frame + 14;

  // Source address
  // Is this the transmitter?
  int this_transmitter = 1;
  int digipeaters = 0;
  // Look for digipeaters
  if(!(frame[13] & 1)){
    // Scan digipeater list; have any repeated it?
    for(int i=0;i<8;i++){
      int digi_ssid = frame[20 + 7*i];
      
      digipeaters++;
      if(digi_ssid & 0x80){
	// Yes, passed this one, keep looking
	this_transmitter = 2 + i;
      }
      if(digi_ssid & 1)
	break; // Last digi
    }
  }
  // Show source address, in upper case if this is the transmitter, otherwise lower case
  for(int n=7; n < 13; n++){
    char c = frame[n] >> 1;
    if(c == ' ')
      break;
    if(this_transmitter == 1)
      putchar(toupper(c));
    else
      putchar(tolower(c));
  }
  printf("-%d",(frame[13] >> 1) & 0xf); // SSID
  
  printf(" -> ");
  
  // List digipeaters

  if(!(frame[13] & 0x1)){
    // Digipeaters are present
    for(int i=0; i<digipeaters; i++){
      for(int k=0;k<6;k++){
	char c = (frame[14 + 7*i + k] >> 1) & 0x7f;
	if(c == ' ')
	  break;
	
	if(this_transmitter == 2+i)
	  putchar(toupper(c));
	else
	  putchar(tolower(c));
      }
      int ssid = frame[14 + 7*i + 6];
      printf("-%d",(ssid>> 1) & 0xf); // SSID
      printf(" -> ");
      if(ssid  & 0x1){ // Last one
	control = frame + 14 + 7*i + 7;
	break;
      }
    }
  }
  // NOW print destination, in lower case since it's not the transmitter
  for(int n=0; n < 6; n++){
    char c = (frame[n] >> 1) & 0x7f;
    if(c == ' ')
      break;
    putchar(tolower(c));
  }
  printf("-%d",(frame[6] >> 1) & 0xf); // SSID

  // Type field
  printf("; control = %02x",*control++ & 0xff);
  printf("; type = %02x\n",*control & 0xff);

  for(int i = 0; i < bytes; i++){
    printf("%02x ",frame[i] & 0xff);
    if((i % 16) == 15 || i == bytes-1){
      for(int k = (i % 16); k < 15; k++)
	printf("   "); // blanks as needed in last line

      printf(" |  ");
      for(int k=(i & ~0xf );k <= i; k++){
	if(frame[k] >= 0x20 && frame[k] < 0x7e)
	  putchar(frame[k]);
	else
	  putchar('.');
      }
      printf("\n");
    }
  }
  printf("\n");
  return 0;
}

// Check CRC-CCITT on frame, return 1 if good, 0 otherwise
int crc_good(unsigned char *frame,int length){
  unsigned int const crc_poly = 0x8408;
	
  unsigned short crc = 0xffff;
  while(length-- > 0){
    unsigned char byte = *frame++;
    for(int i=0; i < 8; i++){
      unsigned short feedback = 0;
      if((crc ^ byte) & 1)
	feedback = crc_poly;

      crc = (crc >> 1) ^ feedback;
      byte >>= 1;
    }
  }
  return(crc == 0xf0b8); // Note comparison
}

int decode_base91(char *in){
  int result = 0;

  for(int i=0;i<4;i++)
    result = 91 * result + in[i] - 33;
  return result;
}
