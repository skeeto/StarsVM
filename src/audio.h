/* audio.h - the WAVEMIX reimplementation's few outside edges. */
#ifndef AUDIO_H
#define AUDIO_H

void api_audio_register(void);

/* Close every device and free every wave.  WaveMixCloseSession calls this, and
   so does main(), because a guest that stops some other way - a fault, --steps,
   the window closing - must not leave a waveform device open. */
void audio_shutdown(void);

/* --play-wave: drive our own WaveMix path against one "WAVE" resource and
   return, without running the guest at all.  Id 0 plays all six overlapping.
   The effects are reachable only from the battle VCR, so this is what separates
   "the audio layer works" from "we can get to a fight". */
int audio_selftest(unsigned id);

#endif
