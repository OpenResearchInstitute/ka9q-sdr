// $Id: misc.h,v 1.6 2018/07/06 06:08:45 karn Exp $
// Miscellaneous constants, macros and function prototypes
// Copyright 2018 Phil Karn, KA9Q
#ifndef _MISC_H
#define _MISC_H 1

int pipefill(int,void *,const int);
void chomp(char *);


// Stolen from the Linux kernel -- enforce type matching of arguments
#define min(x,y) ({			\
		typeof(x) _x = (x);	\
		typeof(y) _y = (y);	\
		(void) (&_x == &_y);	\
		_x < _y ? _x : _y; })

#define max(x,y) ({ \
		typeof(x) _x = (x);	\
		typeof(y) _y = (y);	\
		(void) (&_x == &_y);	\
		_x > _y ? _x : _y; })

#define GPS_UTC_OFFSET (18) // GPS ahead of utc by 18 seconds - make this a table!
#define UNIX_EPOCH ((time_t)315964800) // GPS epoch on unix time scale

char *lltime(long long t);
extern char *Months[12];


// I *hate* this sort of pointless, stupid, gratuitous incompatibility that
// makes a lot of code impossible to read and debug
// The Linux version of pthread_setname_np takes two args, the OSx version only one
// The GNU malloc_usable_size() does exactly the same thing as the BSD/OSX malloc_size()
// except that the former is defined in <malloc.h>, the latter is in <malloc/malloc.h>
#ifdef __APPLE__

#define pthread_setname(x) pthread_setname_np(x)
#include <malloc/malloc.h>
#define malloc_usable_size(x) malloc_size(x)
#else
#include <malloc.h>
#define pthread_setname(x) pthread_setname_np(pthread_self(),x)
#endif


#endif
