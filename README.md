# ka9q-sdr
Package containing several modules including the multicast IP stereo field manager

From Phase 4 Ground Weekly Report for 9 April 2018 (the 409 report):

KA9Q SDR - stereo field

Phil Karn has shared a work in progress with us. He calls it the KA9Q SDR. However, the module in this SDR code that I'd like to highlight is a stereo field audio adapter. 

This works by taking in multicast audio streams. Each audio stream comes from an individual audio source, or participant. These participants in a round table audio conference are placed at different points in the stereo spectrum. 

So, when you are listening to a roundtable or conference, you would hear each voice as if it's coming from a slightly different physical location. It will sound like you are sitting at a table talking to people in front of you. The goal here is to increase intelligibility, reduce listener fatigue, and have fun communicating with others.

The code and early documentation has been uploaded to our repository. (you are here)

Phil has been researching sound localization, and he reports that "it seems that the time delays are actually more important than level differences. The prop delay between the ears is only about 1ms (actually a little less). One site suggests delays up to 10 ms to exaggerate the effect, but I find that the +/- 1ms values sound more realistic. Still trying to figure out the best relationship between delay and gain differences. But overall it works really well. I think this could be a good selling point for a ham voice system based on IP multicast. Each stream arrives independently at the receiver, and because it can tell them apart it can place them at a point in the stereo spectrum chosen by the user."

This module augments our user interface. It provides some advantages but also incurs some additional requirements. It fits in very well with our signal acquisition process. The goal here is to make it "just work", but also be configurable. If you don't want to use this, or can't use it, then you can turn it off. 

