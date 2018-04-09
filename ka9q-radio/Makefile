# $Id: Makefile,v 1.85 2018/02/26 23:01:03 karn Exp $
#CC=g++
INCLUDES=
#COPTS=-g -O2 -DNDEBUG=1 -std=gnu11 -pthread -Wall -funsafe-math-optimizations
COPTS=-g -std=gnu11 -pthread -Wall -funsafe-math-optimizations
CFLAGS=$(COPTS) $(INCLUDES)
BINDIR=/usr/local/bin
LIBDIR=/usr/local/share/ka9q-radio
EXECS=aprs funcube iqplay iqrecord modulate monitor opus opussend packet pcmcat radio
AFILES=bandplan.txt help.txt modes.txt


all: $(EXECS) $(AFILES)

install: all
	install -o root -m 04755 -D --target-directory=$(BINDIR) $(EXECS)
	install -D --target-directory=$(LIBDIR) $(AFILES)

clean:
	rm -f *.o *.a $(EXECS) $(AFILES)
	rcsclean

aprs: aprs.o ax25.o multicast.o misc.o
	$(CC) -g -o $@ $^ -lbsd -lm

packet: packet.o multicast.o filter.o misc.o ax25.o
	$(CC) -g -o $@ $^ -lfftw3f_threads -lfftw3f -lbsd -lm -lpthread 

pcmcat: pcmcat.o multicast.o
	$(CC) -g -o $@ $^ -lbsd

opus: opus.o multicast.o
	$(CC) -g -o $@ $^ -lopus -lbsd -lm -lpthread

opussend: opussend.o multicast.o
	$(CC) -g -o $@ $^ -lopus -lportaudio -lbsd -lm -lpthread $(LD_FLAGS)

control: control.o modes.o
	$(CC) -g -o $@ $^ -lbsd -lm

funcube: funcube.o multicast.o gr.o libfcd.a
	$(CC) -g -o $@ $^ -lasound -lusb-1.0 -lbsd -lm -lpthread

iqplay: iqplay.o multicast.o attr.o misc.o
	$(CC) -g -o $@ $^ -lbsd -lm -lpthread

iqrecord: iqrecord.o multicast.o attr.o
	$(CC) -g -o $@ $^ -lbsd -lm -lpthread

modulate: modulate.o misc.o filter.o 
	$(CC) -g -o $@ $^ -lfftw3f_threads -lfftw3f -lbsd -lm -lpthread

monitor: monitor.o multicast.o
	$(CC) -g -o $@ $^ -lopus -lportaudio -lbsd -lncurses -lm -lpthread

radio: main.o radio.o doppler.o fm.o am.o linear.o filter.o display.o modes.o audio.o multicast.o bandplan.o misc.o knob.o touch.o
	$(CC) -g -o $@ $^ -lfftw3f_threads -lfftw3f -lncurses -lbsd -lm -lpthread

libfcd.a: fcd.o hid-libusb.o
	ar rv $@ $^
	ranlib $@

am.o: am.c misc.h filter.h radio.h audio.h
attr.o: attr.c attr.h
aprs.o: aprs.c multicast.h ax25.h misc.h
audio.o: audio.c misc.h audio.h
ax25.o: ax25.c ax25.h
bandplan.o: bandplan.c bandplan.h
control.o: control.c misc.h radio.h
display.o: display.c radio.h audio.h misc.h filter.h bandplan.h multicast.h
knob.o: knob.c misc.h
touch.o: touch.c misc.h
doppler.o: doppler.c radio.h filter.h misc.h audio.h
fcd.o: fcd.c fcd.h hidapi.h fcdhidcmd.h
filter.o: filter.c misc.h filter.h
fm.o: fm.c misc.h filter.h radio.h audio.h
funcube.o: funcube.c fcd.h fcdhidcmd.h hidapi.h sdr.h radio.h misc.h multicast.h
gr.o: gr.c sdr.h
hid-libusb.o: hid-libusb.c hidapi.h
iqplay.o: iqplay.c misc.h radio.h multicast.h attr.h
iqrecord.o: iqrecord.c radio.h multicast.h attr.h
linear.o: linear.c misc.h filter.h radio.h audio.h
main.o: main.c radio.h filter.h misc.h audio.h multicast.h
misc.o: misc.c radio.h
modes.o: modes.c 
modulate.o: modulate.c misc.h filter.h radio.h
monitor.o: monitor.c misc.h multicast.h
multicast.o: multicast.c multicast.h
opus.o: opus.c misc.h multicast.h	     
opussend.o: opussend.c misc.h multicast.h
packet.o: packet.c filter.h misc.h multicast.h ax25.h
pcmcat.o: pcmcat.c multicast.h
radio.o: radio.c radio.h filter.h misc.h audio.h

