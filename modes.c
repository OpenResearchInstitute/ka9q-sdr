// $Id: modes.c,v 1.25 2018/04/22 22:42:53 karn Exp $
// Load and search mode definition table in /usr/local/share/ka9q-radio/modes.txt
// Copyright 2018, Phil Karn, KA9Q
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <string.h>
#include <errno.h>

#include "misc.h"
#include "radio.h"



#define MAXMODES 256
struct modetab Modes[MAXMODES];
int Nmodes;

extern char Libdir[];

// Linkage table from ascii names to demodulator routines
struct demodtab {
  char name[16];
  void * (*demod)(void *);
} Demodtab[] = {
  {"AM",     demod_am},  // AM evelope detection
  {"FM",     demod_fm},  // NBFM and noncoherent PM
  {"Linear", demod_linear}, // Coherent demodulation of AM, DSB, BPSK; calibration on WWV/WWVH/CHU carrier
};
#define NDEMOD (sizeof(Demodtab)/sizeof(struct demodtab))


int readmodes(char *file){
  char pathname[PATH_MAX];
  snprintf(pathname,sizeof(pathname),"%s/%s",Libdir,file);
  FILE * const fp = fopen(pathname,"r");
  if(fp == NULL){
    fprintf(stderr,"Can't read mode table %s:%s\n",pathname,strerror(errno));
    return -1;
  }
  char line[PATH_MAX];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    // Everything after # is a comment
    char *cp = strchr(line,'#');
    if(cp != NULL)
      *cp = '\0';

    struct modetab *mp = &Modes[Nmodes];

    // Parse line (C is pretty weak on text processing...)
    char *name,*demod;
    char *stringp = line;

    do {
      name = strsep(&stringp," \t");
    } while (name != NULL && *name == '\0');
    
    do {
      demod = strsep(&stringp," \t");
    } while (demod != NULL && *demod == '\0');
    
    if(name == NULL || demod == NULL)
      continue;
    
    int i;
    for(i=0;i<NDEMOD;i++)
      if(strncasecmp(demod,Demodtab[i].name,strlen(Demodtab[i].name)) == 0)
	break;
      
    if(i == NDEMOD)
      continue; // Demod not found in list

    strlcpy(mp->name, name, sizeof(mp->name));
    strlcpy(mp->demod_name, Demodtab[i].name, sizeof(mp->demod_name));
    mp->demod = Demodtab[i].demod;

    double low,high;
    low = strtod(stringp,&stringp);
    high = strtod(stringp,&stringp);
    if(high < low){ // Ensure high > low
      mp->low = high;
      mp->high = low;
    } else {
      mp->low = low;
      mp->high = high;
    }
    mp->shift = strtod(stringp,&stringp);
    mp->attack_rate = -fabs(strtod(stringp,&stringp));
    mp->recovery_rate = fabs(strtod(stringp,&stringp));
    mp->hangtime = fabs(strtod(stringp,&stringp)); // Must be positive

    // Process options
    mp->flags = 0;
    for(int i=0;i<8;i++){
      char *option;
      // Skip leading space
      do {
	option = strsep(&stringp," \t");
      } while (option != NULL && *option == '\0');
      if(option == NULL)
	break; // No more

      if(strcasecmp(option,"isb") == 0 || strcasecmp(option,"conj") == 0){
	mp->flags |= ISB;         // For independent sideband: LSB on left, USB on right
      } else if(strcasecmp(option,"flat") == 0){
	mp->flags |= FLAT;         // FM only
      } else if(strcasecmp(option,"cal") == 0){
	mp->flags |= CAL|PLL; // Calibrate implies PLL
      } else if(strcasecmp(option,"square") == 0){
	mp->flags |= SQUARE|PLL; // Square implies PLL
      } else if(strcasecmp(option,"coherent") == 0 || strcasecmp(option,"pll") == 0){
	mp->flags |= PLL;
      } else if(strcasecmp(option,"envelope") == 0){
	mp->flags |= ENVELOPE | MONO; // Envelope detected AM implies mono
      } else if(strcasecmp(option,"mono") == 0){
	mp->flags |= MONO;  // E.g., if you don't want the hilbert transform of SSB on the right channel
      }
    }    
    Nmodes++;
    if(Nmodes == MAXMODES)
      break; // table full
  }
  return 0;
}

