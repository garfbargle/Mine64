#ifndef AUDIO_H
#define AUDIO_H

#include "menu.h"

enum SoundEffect {
  SOUND_PICKUP,
  SOUND_PUNCH,
  SOUND_BREAK,
  SOUND_PLACE
};

/*
 * Loudness, as steps rather than a raw amplitude: the options tab draws these
 * as a row of pips and the player counts them, so the scale has to be short
 * enough to count and the endpoints have to mean something.  Step 0 is
 * silence, the last step is the level the game shipped at.
 *
 * The count is declared outside ENABLE_AUDIO because the tab that draws the
 * pips is built either way; only the calls that would reach the audio library
 * are compiled out.
 */
#define AUDIO_VOLUME_STEPS 5
#define AUDIO_VOLUME_DEFAULT (AUDIO_VOLUME_STEPS - 1)

#ifdef ENABLE_AUDIO
void initAudio(void);
void updateAudio(enum Screen screen);
void playSound(enum SoundEffect effect);
/*
 * Music takes effect on the voice already playing rather than restarting it,
 * which is the whole reason silence is volume zero here instead of a stop:
 * stopping deallocates the voice and playTrack would start the song again
 * from its first bar the moment the player turned it back up.
 */
void setMusicVolume(u8 step);
void setSoundVolume(u8 step);
u8 musicVolume(void);
u8 soundVolume(void);
/* Peak KiB taken from the audio heap, for the diagnostic overlay's U row.  It
   read 64 on hardware, which is what MINE64_AU_HEAP_SIZE's 70 KiB is sized
   against.  See docs/ram-budget.md. */
u32 audioHeapPeakKiB(void);
#else
#define playSound(effect) ((void)(effect))
/* A build with no audio still draws the rows, so they answer honestly: the
   volume is zero and setting it does nothing. */
#define setMusicVolume(step) ((void)(step))
#define setSoundVolume(step) ((void)(step))
#define musicVolume() ((u8) 0)
#define soundVolume() ((u8) 0)
#endif

#endif /* AUDIO_H */
