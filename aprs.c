// $Id: aprs.c,v 1.7 2018/04/23 09:51:53 karn Exp $
// Process AX.25 frames containing APRS data, extract lat/long/altitude, compute az/el
// INCOMPLETE, doesn't yet drive antenna rotors
// Should also use RTP for AX.25 frames
// Should get station location from a GPS receiver
// Copyright 2018, Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>
#include <time.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"

char *Mcast_address_text = "ax25.vhf.mcast.local:8192";
char *Source = "W6SUN-4";

double const WGS84_E = 0.081819190842622;  // Eccentricity
double const WGS84_A = 6378137;         // Equatorial radius, meters

int Verbose;
int Input_fd = -1;
int All;

#define square(x) ((x)*(x))

int main(int argc,char *argv[]){
  setlocale(LC_ALL,getenv("LANG"));
  double latitude = NAN;
  double longitude = NAN;
  double altitude = NAN;

  int c;
  while((c = getopt(argc,argv,"L:M:A:I:vs:a")) != EOF){
    switch(c){
    case 'a':
      All++;
      break;
    case 'L':
      latitude = strtod(optarg,NULL);
      break;
    case 'M':
      longitude = strtod(optarg,NULL);
      break;
    case 'A':
      altitude = strtod(optarg,NULL);
      break;
    case 's':
      Source = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      Mcast_address_text = optarg;
      break;
    default:
      fprintf(stderr,"Usage: %s [-v] [-I mcast_address]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -I %s\n",argv[0],Mcast_address_text);
      exit(1);
    }
  }
  if(isnan(latitude) && isnan(longitude) && isnan(altitude)){
#if 0
    // Use defaults - KA9Q location, be sure to change elsewhere!!
    latitude = 32.8604;
    longitude = -117.1889;
    altitude = 0;
#else
    // MCHSARC
    latitude = 32.967233;
    longitude = -117.122382;
    altitude = 200;
#endif
  } else if(isnan(latitude) || isnan(longitude) || isnan(altitude)){
    fprintf(stderr,"Must supply all three of -L latitude -M longitude -A altitude\n");
    exit(1);
  }
  printf("APRS az/el program by KA9Q\n");
  if(All){
    printf("Watching all stations\n");
  } else {
    printf("Watching for %s\n",Source);
  }
  printf("Station coordinates: longitude %.6lf deg; latitude %.6lf deg; altitude %.1lf m\n",
	 longitude,latitude,altitude);

  // Station position in earth-centered ROTATING coordinate system
  double station_x,station_y,station_z;
  // Unit vectors defining station's orientation
  double up_x,up_y,up_z;
  double south_x,south_y,south_z;  
  double east_x,east_y,east_z;
  
  {
    double sinlat,coslat;
    sincos(RAPDEG*latitude,&sinlat,&coslat);
    double sinlong,coslong;
    sincos(RAPDEG*longitude,&sinlong,&coslong);
    
    double tmp = WGS84_A/sqrt(1-(square(WGS84_E)*square(sinlat)));
    station_x = (tmp + altitude) * coslat * coslong;
    station_y = (tmp + altitude) * coslat * sinlong;
    station_z = (tmp*(1-square(WGS84_E)) + altitude) * sinlat;
    
    // Zenith vector is (coslong*coslat, sinlong*coslat, sinlat)
    up_x = coslong * coslat;
    up_y = sinlong * coslat;
    up_z = sinlat;

    east_x = -sinlong;
    east_y = coslong;
    east_z = 0;

    south_x = coslong*sinlat;
    south_y = sinlong*sinlat;
    south_z = -(sinlong*sinlong*sinlat + coslong*coslong*coslat);
  }    

  // Set up multicast input
  Input_fd = setup_mcast(Mcast_address_text,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up input from %s\n",
	    Mcast_address_text);
    exit(1);
  }
  unsigned char packet[2048];
  int len;

  while((len = recv(Input_fd,packet,sizeof(packet),0)) > 0){
    time_t t;
    struct tm *tmp;
    time(&t);
    tmp = gmtime(&t);
    printf("%d %s %04d %02d:%02d:%02d UTC: ",tmp->tm_mday,Months[tmp->tm_mon],tmp->tm_year+1900,
	   tmp->tm_hour,tmp->tm_min,tmp->tm_sec);
		

    //    dump_frame(packet,len);
    // Is this the droid we're looking for?
    char result[10];

    get_callsign(result,packet+7);
    printf("source = %s\n",result);
    if(All || strncasecmp(result,Source,sizeof(result)) == 0){
      // Find end of address field
      int i;
      for(i = 0; i < len;i++){
	if(packet[i] & 1)
	  break;
      }
      if(i == len){
	// Invalid
	printf("Incomplete frame\n");
	continue;
      }
      if(packet[i+1] != 0x03 || packet[i+2] != 0xf0){
	printf("Invalid ax25 type\n");
	continue;
      }
      char *data = (char *)(packet + i + 3); // First byte of text field
      // Extract lat/long

      // Parse APRS position packets
      // The APRS spec is an UNBELIEVABLE FUCKING MESS THAT SHOULD BE SHOT, SHREDDED, BURNED AND SENT TO HELL!
      // There, now I feel a little better. But not much.
      double latitude=NAN,longitude=NAN,altitude=NAN;
      int hours=-1, minutes=-1,days=-1,seconds=-1;

      // Sample WB8ELK LU1ESY-3>APRS,TCPIP*,qAS,WB8ELK:/180205h3648.75S/04627.50WO000/000/A=039566 2 4.50 25 12060 GF63SE 1N7MSE 226
      // Sample PITS "!/%s%sO   /A=%06ld|%s|%s/%s,%d'C,http://www.pi-in-the-sky.com",

      if(*data == '/' || *data == '@'){
	// process timestamp
	char *ncp = NULL;
	data++;
	int t = strtol(data,&ncp,10);
	if(*ncp == 'h'){
	  // Hours, minutes, seconds
	  days = 0;
	  hours = t / 10000;
	  t -= hours * 10000;
	  minutes = t / 100;
	  t -= minutes * 100;
	  seconds = t;
	} else if(*ncp == 'z'){
	  // day, hours minutes zulo
	  days = t / 10000;
	  t -= days * 10000;
	  hours = t / 100;
	  t -= hours * 100;
	  minutes = t;
	  seconds = 0;
	} else if(*ncp == '/'){
	  // day, hours, minutes local -- HOW AM I SUPPOSED TO KNOW THE TIME ZONE ??!?!?
	  days = t / 10000;
	  t -= days * 10000;
	  hours = t / 100;
	  t -= hours * 100;
	  minutes = t;
	  seconds = 0;
	}

	data = ncp+1; // skip 'h' or 'z' (process?)
      } else if(*data == '!' || *data == '='){
	// Position without timestamp
	data++;
      } else {
	printf("Unsupported APRS frame type 0x%x (%c)\n",*data,*data);
	continue;
      }
	      
      // parse position
      if(*data == '/'){
	// Compressed
	data++; // skip /
	latitude = 90 - decode_base91(data)/380926.;
	longitude = -180 + decode_base91(data+4) / 190463.;
	data += 12;
      } else if(isdigit(*data)){
	// Uncompressed
	char *ncp = NULL;
	latitude = strtod(data,&ncp) / 100.;
	latitude = (int)(latitude) + fmod(latitude,1.0) / 0.6;
	if(tolower(*ncp) == 's')
	  latitude = -latitude;
	data = ncp + 2; // Skip S and /
	longitude = strtod(data,&ncp) / 100.;
	longitude = (int)(longitude) + fmod(longitude,1.0) / 0.6;
	if(tolower(*ncp) == 'w')
	  longitude = -longitude;
	data = ncp + 2; // Skip W and /
	// Look for A=
	while(*data != '\0' && *(data+1) != '\0'){
	  if(*data == 'A' && data[1] == '='){
	    altitude = strtol(data+2,&ncp,10);
	    break;
	  } else
	    data++;
	}
	altitude *= 0.3048; // Convert to meters
      } else {
	printf("Unparseable position report\n");
	continue;
      }
      if(days != -1 || hours != -1 || minutes != -1 || seconds != -1)
	printf("days %d hours %d minutes %d seconds %d\n",days,hours,minutes,seconds);
      printf("Latitude %.6lf deg; Longitude %.6lf deg; Altitude %.1lf m\n",latitude,longitude,altitude);
      if(isnan(altitude))
	altitude = 0;

      double balloon_x,balloon_y,balloon_z;
      {
	double sinlat,coslat;
	sincos(RAPDEG*latitude,&sinlat,&coslat);
	double sinlong,coslong;
	sincos(RAPDEG*longitude,&sinlong,&coslong);
      
	double tmp = WGS84_A/sqrt(1-(square(WGS84_E)*square(sinlat))); // Earth radius under target
	balloon_x = (tmp + altitude) * coslat * coslong;
	balloon_y = (tmp + altitude) * coslat * sinlong;
	balloon_z = (tmp*(1-square(WGS84_E)) + altitude) * sinlat;
      }
      double look_x,look_y,look_z;
      look_x = balloon_x - station_x;
      look_y = balloon_y - station_y;
      look_z = balloon_z - station_z;      
      double range = sqrt(square(look_x)+square(look_y)+square(look_z));
      
      double south = (south_x * look_x + south_y * look_y + south_z * look_z) / range;
      double east = (east_x * look_x + east_y * look_y + east_z * look_z) / range;
      double up = (up_x * look_x + up_y * look_y + up_z * look_z) / range;
      

      double elevation = asin(up);
      double azimuth = M_PI - atan2(east,south);


      printf("azimuth %.1lf deg, elevation %.1lf deg, range %.1lf m\n",
	     azimuth*DEGPRA, elevation*DEGPRA,range);

    }
  }
}

