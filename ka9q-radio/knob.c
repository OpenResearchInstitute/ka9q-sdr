#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <fcntl.h>
#include <linux/input.h>

#include "misc.h" // for our version of pthread_setname()

#define DIAL "/dev/input/by-id/usb-Griffin_Technology__Inc._Griffin_PowerMate-event-if00"

extern void adjust_up(void *);
extern void adjust_down(void *);
extern void toggle_lock(void *);

// Look for a powermate tuning knob
void *knob(void *arg){
  if(arg == NULL)
    return NULL;

  pthread_setname("knob");
  int dial_fd = -1;

  while(1){
    if(dial_fd == -1
       && (dial_fd = open(DIAL,O_RDONLY)) == -1){
      usleep(1000000); // Don't spin tight
      continue;
    }
    struct input_event event;
    int len;
    if((len = read(dial_fd,&event,sizeof(event))) == -1){
      dial_fd = -1; // Close and retry
      continue;
    }
    // Got something from the powermate knob
    // ignore event.type == EV_SYN; all knob events are self contained
    if(event.type == EV_REL){
      if(event.code == REL_DIAL){
	// Dial has been turned. Simulate up/down arrow tuning commands
	if(event.value > 0){
	  adjust_up(arg);
	} else if(event.value < 0)
	  adjust_down(arg);
      }
    } else if(event.type == EV_KEY){
      if(event.code == BTN_MISC)
	if(event.value){
	  toggle_lock(arg);
	}
    }
  }
  return NULL;
}
