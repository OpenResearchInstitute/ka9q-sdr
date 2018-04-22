#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <fcntl.h>
#include <linux/input.h>


#include "misc.h" // for our version of pthread_setname()
#define TOUCH "/dev/input/event1"

void touchitem(void *arg,int x,int y,int ev);

void *touch(void *arg){
  if(arg == NULL)
    return NULL;

  pthread_setname("touch");
  int touch_fd = -1;

  int pos_x=0,pos_y=0,pos_id=0;
  while(1){
    if(touch_fd == -1
       && (touch_fd = open(TOUCH,O_RDONLY)) == -1){
      usleep(1000000); // Don't spin tight
      continue;
    }
    struct input_event event;
    int len;
    if((len = read(touch_fd,&event,sizeof(event))) == -1){
      touch_fd = -1; // Close and retry
      continue;
    }

    // Got something from the touchscreen
    if(event.type == EV_SYN){
      touchitem(arg,pos_x,pos_y,pos_id);
      continue;
    }
    if(event.type == EV_ABS){
      switch(event.code){
      case ABS_MT_TRACKING_ID:
	pos_id = event.value;
	break;
      case ABS_MT_POSITION_X:
	pos_x = event.value;
	break;
      case ABS_MT_POSITION_Y:
	pos_y = event.value;
      default:
	break;
      }
    }
  }
}

