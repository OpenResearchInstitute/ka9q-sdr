# $Id: Makefile,v 1.93 2018/04/20 08:14:34 karn Exp karn $
#CC=g++
INCLUDES=
COPTS=-g -O2 -DNDEBUG=1 -std=gnu11 -pthread -Wall -funsafe-math-optimizations
#COPTS=-g -std=gnu11 -pthread -Wall -funsafe-math-optimizations
CFLAGS=$(COPTS) $(INCLUDES)
BINDIR=/usr/local/bin
LIBDIR=/usr/local/share/ka9q-radio
EXECS=aprs funcube iqplay iqrecord modulate monitor opus opussend packet radio pcmsend
AFILES=bandplan.txt help.txt modes.txt


all: $(EXECS) $(AFILES)

install: all
	install -o root -m 04755 -D --target-directory=$(BINDIR) $(EXECS)
	install -D --target-directory=$(LIBDIR) $(AFILES)

clean:
	rm -f *.o *.a $(EXECS) $(AFILES)

.PHONY: clean all


# Executables
aprs: aprs.o ax25.o libradio.a
	$(CC) -g -o $@ $^ -lbsd -lm

packet: packet.o libradio.a
	$(CC) -g -o $@ $^ -lfftw3f_threads -lfftw3f -lbsd -lm -lpthread 

opus: opus.o libradio.a
	$(CC) -g -o $@ $^ -lopus -lbsd -lm -lpthread

opussend: opussend.o libradio.a
	$(CC) -g -o $@ $^ -lopus -lportaudio -lbsd -lm -lpthread

pcmsend: pcmsend.o libradio.a
	$(CC) -g -o $@ $^ -lportaudio -lbsd -lm -lpthread

funcube: funcube.o libradio.a libfcd.a
	$(CC) -g -o $@ $^ -lasound -lusb-1.0 -lbsd -lm -lpthread

iqplay: iqplay.o libradio.a
	$(CC) -g -o $@ $^ -lbsd -lm -lpthread

iqrecord: iqrecord.o libradio.a
	$(CC) -g -o $@ $^ -lbsd -lm -lpthread

modulate: modulate.o libradio.a
	$(CC) -g -o $@ $^ -lfftw3f_threads -lfftw3f -lbsd -lm -lpthread

monitor: monitor.o libradio.a
	$(CC) -g -o $@ $^ -lopus -lportaudio -lbsd -lncursesw -lm -lpthread

radio: main.o libradio.a
	$(CC) -g -o $@ $^ -lfftw3f_threads -lfftw3f -lncursesw -lbsd -lm -lpthread

# Binary libraries
libfcd.a: fcd.o hid-libusb.o
	ar rv $@ $^
	ranlib $@

libradio.a: am.o attr.o audio.o ax25.o bandplan.o display.o doppler.o filter.o fm.o gr.o knob.o linear.o misc.o modes.o multicast.o radio.o touch.o
	ar rv $@ $^
	ranlib $@

# Main programs
aprs.o: aprs.c ax25.h multicast.h misc.h
funcube.o: funcube.c fcd.h fcdhidcmd.h hidapi.h sdr.h radio.h misc.h multicast.h
iqplay.o: iqplay.c misc.h radio.h sdr.h multicast.h attr.h
iqrecord.o: iqrecord.c radio.h sdr.h multicast.h attr.h
main.o: main.c radio.h sdr.h filter.h misc.h audio.h multicast.h
modulate.o: modulate.c misc.h filter.h radio.h sdr.h
monitor.o: monitor.c misc.h multicast.h
opus.o: opus.c misc.h multicast.h
opussend.o: opussend.c misc.h multicast.h
pcmsend.o: pcmsend.c misc.h multicast.h


# Components of libfcd.a
fcd.o: fcd.c fcd.h hidapi.h fcdhidcmd.h
hid-libusb.o: hid-libusb.c hidapi.h

# Components of libradio.a
am.o: am.c misc.h filter.h radio.h audio.h sdr.h
attr.o: attr.c attr.h
audio.o: audio.c misc.h audio.h multicast.h
ax25.o: ax25.c ax25.h
bandplan.o: bandplan.c bandplan.h
display.o: display.c radio.h sdr.h audio.h misc.h filter.h bandplan.h multicast.h
doppler.o: doppler.c radio.h sdr.h misc.h
filter.o: filter.c misc.h filter.h
fm.o: fm.c misc.h filter.h radio.h sdr.h audio.h
gr.o: gr.c sdr.h
knob.o: knob.c misc.h
linear.o: linear.c misc.h filter.h radio.h sdr.h audio.h
misc.o: misc.c radio.h sdr.h
modes.o: modes.c radio.h sdr.h misc.h
multicast.o: multicast.c multicast.h
packet.o: packet.c filter.h misc.h multicast.h ax25.h
radio.o: radio.c radio.h sdr.h filter.h misc.h audio.h 
touch.o: touch.c misc.h
