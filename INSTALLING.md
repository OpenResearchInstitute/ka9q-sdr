# Installing ka9q-sdr

The ka9q-sdr package is primarily designed for Debian Linux,
including the Raspbian variant for the Raspberry Pi.

Most but not all of the programs will also build and run on Mac OSX,
mainly because I have a MacBook Pro as my primary desktop.

The exception is the 'funcube' program that reads an I/Q data stream
from an AMSAT UK FUNcube Pro+ dongle and multicasts it to the
network. It uses the Linux ALSA sound interface directly while the
other programs use the portaudio19 shim. I plan to migrate 'funcube'
to portaudio, but it also sends commands to the dongle with a Linux
USB library that might not port to OSX.

The ka9q-sdr package has several package dependencies beyond those
usually needed to compile C programs (e.g., make, gcc).

## Raspbian, Debian or Ubuntu

On Raspbian, Debian or Ubuntu Linux, run the following command (as root):

```
apt install build-essential libfftw3-dev libbsd-dev libopus-dev libusb-1.0-0-dev \
    libasound2-dev libncursesw5-dev libattr1-dev portaudio19-dev libncurses5-dev
```
This is known to work on Ubuntu 18.04 LTS and Debian 9 (Stretch). It will *not* work 
with Ubuntu 14.04 LTS, Ubuntu 16.04 LTS or Debian 8 (Jessie). The reason is that 
ka9q-sdr requires at least version 3.3.5 of libfftw3-dev, and these distributions 
use an older version. The symptom here is that at link-time, the symbol 
`fftwf_make_planner_thread_safe()` is missing.

As a workaround, you can install and build it from the upstream 
source at http://www.fftw.org/fftw-3.3.8.tar.gz (Thanks PY2SDR)

Additionally, compiles on Ubuntu 14.04 LTS fail, because it ships with
a version of gcc (4.8) that does not support the include file
`stdatomic.h`.

We are working on better build instructions that reflect these issues.

## macOS

On macOS, you'll need Apple's Xcode developer package with the command
line tools and the third-party 'macports' package, https://www.macports.org.

Then run, as root:

```
port install fftw-3 libopus portaudio
```

I haven't set up virgin systems to test the installs so I could easily have missed something. If so, please let me know or file an issue.

Phil Karn

