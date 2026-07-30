#ifndef AUDIO_H
#define AUDIO_H

#include "menu.h"

/* Initialize the single-voice music player after NuSystem graphics is ready. */
void initAudio(void);

/* Keep the title and gameplay tracks in sync with the current screen. */
void updateAudio(enum Screen screen);

#endif /* AUDIO_H */
