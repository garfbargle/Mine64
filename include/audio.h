#ifndef AUDIO_H
#define AUDIO_H

#include "menu.h"

#ifdef ENABLE_AUDIO
void initAudio(void);
void updateAudio(enum Screen screen);
#endif

#endif /* AUDIO_H */
