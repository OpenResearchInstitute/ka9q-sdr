// $Id: gr.c,v 1.6 2017/05/20 10:10:15 karn Exp $
// Mirics default gain tables
// Mirics SDR API specification, section 5
// Return actual gain, which can be different from parameter if out of range
#include <stdint.h>
#include "sdr.h"


int mirics_gain(double f,int g,uint8_t *bb, uint8_t *lna,uint8_t *mix){
  if(g < 0)
    g = 0;

  if(g < 20){
    // At low gain, use the IF amplifier for all bands
    *lna = 0;
    *mix = 0;
    *bb = g;
  } else if((f < 60e6 && g < 68)|| (f < 250e6 && g < 74) || (f < 420e6 && g < 68) || (f < 1e9 && g < 74) || (g < 76)){
    // Turn on 19 dB mixer, do rest with IF
    *lna = 0;
    *mix = 1;
    *bb = g - 19;
  } else {
    //Turn on everything
    *mix = 1;
    *lna = 1;
    *bb = g - 19;
    if(f < 420e6)
      *bb -= 24;
    else
      *bb -= 7;
  }
  if(*bb > 59){ // Limit to legal range
    g -= *bb - 59;
    *bb = 59;
  }
  return g;
}
