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
#else
#define playSound(effect) ((void)(effect))
#endif

#endif /* AUDIO_H */
