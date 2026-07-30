#include <nusys.h>
#include <nualsgi.h>
#include "audio.h"
#include "music_title_vadpcm.h"
#include "music_game_vadpcm.h"

/* These are ROM offsets emitted by the two RAW segments in spec. */
extern u8 _musicTitleSegmentRomStart[];
extern u8 _musicGameSegmentRomStart[];

typedef struct {
  s32 order;
  s32 predictors;
  /* VADPCM permits up to 16 predictors with 8 coefficients each. */
  s16 coefficients[16 * 8];
} MusicBook;

typedef struct {
  ALWaveTable wave;
  ALSound sound;
  MusicBook book;
  ALADPCMloop loop;
} MusicTrack;

static ALEnvelope music_envelope = {
  0, -1, 0, AL_VOL_FULL, AL_VOL_FULL
};

static ALKeyMap music_keymap = {
  0, 127, 0, 127, 60, 0
};

static MusicTrack title_track = {
  {
    _musicTitleSegmentRomStart,
    MUSIC_TITLE_VADPCM_DATA_BYTES,
    AL_ADPCM_WAVE,
    1,
    { .adpcmWave = { NULL, NULL } }
  },
  { &music_envelope, &music_keymap, NULL, AL_PAN_CENTER, AL_VOL_FULL, 1 },
  {
    MUSIC_TITLE_VADPCM_BOOK_ORDER,
    MUSIC_TITLE_VADPCM_BOOK_PREDICTORS,
    MUSIC_TITLE_VADPCM_BOOK_VALUES
  },
  { 0, MUSIC_TITLE_VADPCM_SAMPLE_COUNT, (u32)-1, { 0 } }
};

static MusicTrack game_track = {
  {
    _musicGameSegmentRomStart,
    MUSIC_GAME_VADPCM_DATA_BYTES,
    AL_ADPCM_WAVE,
    1,
    { .adpcmWave = { NULL, NULL } }
  },
  { &music_envelope, &music_keymap, NULL, AL_PAN_CENTER, AL_VOL_FULL, 1 },
  {
    MUSIC_GAME_VADPCM_BOOK_ORDER,
    MUSIC_GAME_VADPCM_BOOK_PREDICTORS,
    MUSIC_GAME_VADPCM_BOOK_VALUES
  },
  { 0, MUSIC_GAME_VADPCM_SAMPLE_COUNT, (u32)-1, { 0 } }
};

static ALSndId active_music = -1;
static MusicTrack *current_track = NULL;

static void prepareTrack(MusicTrack *track) {
  track->wave.waveInfo.adpcmWave.loop = &track->loop;
  track->wave.waveInfo.adpcmWave.book = (ALADPCMBook *)&track->book;
  track->sound.wavetable = &track->wave;
}

static void playTrack(MusicTrack *track) {
  if (track == current_track) {
    return;
  }
  if (active_music != -1) {
    alSndpSetSound(&nuAuSndPlayer, active_music);
    alSndpStop(&nuAuSndPlayer);
    alSndpDeallocate(&nuAuSndPlayer, active_music);
  }
  active_music = alSndpAllocate(&nuAuSndPlayer, &track->sound);
  if (active_music == -1) {
    return;
  }
  alSndpSetSound(&nuAuSndPlayer, active_music);
  alSndpSetPitch(&nuAuSndPlayer, 1.0f);
  alSndpSetPan(&nuAuSndPlayer, AL_PAN_CENTER);
  alSndpSetVol(&nuAuSndPlayer, 24576);
  alSndpPlay(&nuAuSndPlayer);
  current_track = track;
}

void initAudio(void) {
  /* One looping music voice is all Mine64 currently needs. Smaller DMA and
     command buffers leave RAM/RSP time for the split-screen renderer. */
  nuAuSynConfig.maxVVoices = 1;
  nuAuSynConfig.maxPVoices = 1;
  nuAuSynConfig.maxUpdates = 16;
  nuAuSynConfig.outputRate = MUSIC_TITLE_VADPCM_SAMPLE_RATE;
  nuAuSndpConfig.maxSounds = 1;
  nuAuSndpConfig.maxEvents = 4;
  nuAuDmaBufNum = 16;
  nuAuDmaBufSize = 2048;
  nuAuAcmdLen = 0x200;

  prepareTrack(&title_track);
  prepareTrack(&game_track);
  nuAuMgrInit((void *)NU_AU_HEAP_ADDR, 0x20000, &nuAuSynConfig);
  nuAuSndPlayerInit(&nuAuSndpConfig);
  nuAuPreNMIFuncSet(nuAuPreNMIProc);
}

void updateAudio(enum Screen screen) {
  if (screen == GAME || screen == INVENTORY) {
    playTrack(&game_track);
  } else {
    playTrack(&title_track);
  }
}
