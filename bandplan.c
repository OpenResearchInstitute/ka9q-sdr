#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <ctype.h>
#include <math.h>
#include <limits.h>

#include "radio.h"
#include "bandplan.h"

char Bandplan_file[] = "bandplan.txt";
#define MAX_BANDPLANS 1000
struct bandplan Bandplans[MAX_BANDPLANS];
int Nbandplans;


static int compar(void const *a,void const *b){
  const double f = *(double *)a;
  const struct bandplan *bp = b;

  if(f < bp->lower)
    return -1;
  if(f > bp->upper)
    return 1;
  else
    return 0;
}

int Bandplan_init;
extern int init_bandplan(void);

struct bandplan *lookup_frequency(double f){
  double key;

  key = round(f) / 1.0e6;

  if(!Bandplan_init){
    init_bandplan();
    Bandplan_init = 1;
  }
  return bsearch(&key,Bandplans,Nbandplans,sizeof(struct bandplan),compar);
}


int init_bandplan(){
  char fname[PATH_MAX];

  snprintf(fname,sizeof(fname),"%s/%s",Libdir,Bandplan_file);

  FILE * const fp = fopen(fname,"r");
  if(fp == NULL)
    return -1;

  char line[160];
  memset(line,0,sizeof(line));
  int i=0;
  for(i=0; i < MAX_BANDPLANS && fgets(line,sizeof(line),fp) != NULL;){
    if(line[0] == ';' || line[0] == '#')
      continue;
    char classes[160];
    char modes[160];
    double lower,upper;
    int nchar = 0;
    int r = sscanf(line,"%lf %lf %160s %160s %n",&lower,&upper,classes,modes,&nchar);
    if(r < 4){
      double center,bw;
      r = sscanf(line,"%lf b%lf %160s %160s %n",&center,&bw,classes,modes,&nchar);      
      if(r < 4)
	continue;
      lower = center - bw/2;
      upper = center + bw/2;
    }

    memset(&Bandplans[i],0,sizeof(struct bandplan));
    Bandplans[i].lower = lower;
    Bandplans[i].upper = upper;
    for(char *cp = classes;*cp != '\0';cp++){
      switch(tolower(*cp)){
      case '-':
	Bandplans[i].classes = 0; // No privileges
	break;
      case 'e':
	Bandplans[i].classes |= EXTRA_CLASS;
	break;
      case 'a':
	Bandplans[i].classes |= ADVANCED_CLASS;
	break;
      case 'g':
	Bandplans[i].classes |= GENERAL_CLASS;
	break;
      case 't':
	Bandplans[i].classes |= TECHNICIAN_CLASS;
	break;
      case 'n':
	Bandplans[i].classes |= NOVICE_CLASS;
	break;
      }
    }
    for(char *cp = modes;*cp != '\0';cp++){
      switch(tolower(*cp)){
      case '-':
	Bandplans[i].modes = 0; // No modes!
	break;
      case 'c':
	Bandplans[i].modes |= CW;
	break;
      case 'v':
	Bandplans[i].modes |= VOICE;
	break;
      case 'i':
	Bandplans[i].modes |= IMAGE;
	break;
      case 'd':
	Bandplans[i].modes |= DATA;
	break;
      }
    }    
    strlcpy(Bandplans[i].name,line + nchar,sizeof(Bandplans[i].name));
    i++;
  }
#if 0
  fprintf(stderr,"%d entries read\n",i);
#endif
  Nbandplans = i;
  return 0;
}
#if 0
int main(){
  double f;
  struct bandplan const *bp;

  while(1){
    scanf("%lf",&f);
    bp = lookup_frequency(f);
    if(bp != NULL){
      printf("%ld: %lf",f,bp->wavelength);
      if(bp->modes & CW)
	printf(" CW");
      if(bp->modes & DATA)
	printf(" Data");
      if(bp->modes & VOICE)
	printf(" Voice");
      if(bp->modes & IMAGE)
	printf(" Image");
      printf("\n");
    }
  }
}

#endif
