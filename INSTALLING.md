The ka9q-radio package is primarily designed for Debian Linux,
including the Raspbian variant for the Raspberry Pi.

Most of the programs will also build and run on Mac OSX,
mainly because I have a MacBook Pro as my primary desktop.

The ka9q-radio package has several package dependencies beyond those
usually needed to compile C programs (e.g., make, gcc).

On Raspbian, Debian or Ubuntu Linux (>= 16.04 LTS), run the following command (as root):

```
apt install libfftw3-dev libbsd-dev libopus-dev libusb-1.0-0-dev libasound2-dev \ 
       libncurses5-dev libncursesw5-dev libattr1-dev portaudio19-dev libhackrf-dev
```
Please note that this will not work on Ubuntu 14.04 LTS, as its version of gcc is too old.

Some versions of Ubuntu (e.g. 16.04 LTS) have an older version of libfftw3-dev that lacks full
thread safety. The symptom here is the missing link-time symbol
`fftwf_make_planner_thread_safe()`. If necessary, you can install and build it from the upstream source at

   http://www.fftw.org/fftw-3.3.7.tar.gz

(Thanks PY2SDR)

On Mac OSX, you'll need Apple's Xcode developer package with the command
line tools and the third-party 'macports' package, https://www.macports.org.

Then run, as root:

port install fftw-3 libopus portaudio hackrf-devel hackrf ncurses

I haven't set up virgin systems to test the installs so I could easily have missed something. If so, please let me know.

Phil Karn, KA9Q
Updated 8 Sept 2018
