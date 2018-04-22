The ka9q-radio package is primarily designed to run on Debian Linux,
including the Raspbian variant for the Raspberry Pi.

Most but not all of the programs will also build and run on Mac OSX,
mainly because I have a MacBook Pro that serves as my primary
desktop. The exception is the 'funcube' program that reads an I/Q data
stream from an AMSAT UK FUNcube Pro+ dongle and multicasts it to the
network. This is mainly because it uses the Linux ALSA sound interface
directly while the other programs use the portaudio19 shim. I plan to
migrate 'funcube' to portaudio, but it also sends commands to the
dongle with a Linux USB library that might not port to OSX.

The ka9q-radio package has the following build dependencies beyond the
usual ones needed to compile C programs (e.g., make, gcc).

On Debian Linux:

libfftw3-dev
libbsd-dev
libopus-dev
libusb-1.0-0-dev
libasound2-dev
libncursesw5-dev (note: this is the 'wide' version that supports Unicode graphical characters)
libattr1-dev
portaudio19-dev

On Mac OSX, you'll need the Xcode developer package with command line
tools and the third-party 'macports' package
(https://www.macports.org/) with several needed libraries:

fftw-3
libopus
portaudio
