#ifndef AUDIO_H
#define AUDIO_H

#include "menu.h"

enum SoundEffect {
  SOUND_PICKUP,
  SOUND_PUNCH,
  SOUND_BREAK,
  SOUND_PLACE
};

#ifdef ENABLE_AUDIO
void initAudio(void);
void updateAudio(enum Screen screen);
void playSound(enum SoundEffect effect);
/* Peak KiB taken from the audio heap, for the diagnostic overlay's U row.  It
   read 64 on hardware, which is what MINE64_AU_HEAP_SIZE's 70 KiB is sized
   against.  See docs/ram-budget.md. */
u32 audioHeapPeakKiB(void);
#else
#define playSound(effect) ((void)(effect))
#endif

#endif /* AUDIO_H */
