#include <nusys.h>

#ifdef ENABLE_AUDIO

#include <nualsgi.h>
#include "audio.h"
#include "music_title_vadpcm.h"
#include "music_game_vadpcm.h"

extern u8 _musicTitleSegmentRomStart[];
extern u8 _musicGameSegmentRomStart[];

typedef struct {
  s32 order;
  s32 predictors;
  s16 coefficients[16 * 8];
} MusicBook;

typedef struct {
  ALWaveTable *wave;
  ALSound *sound;
  ALADPCMBook *book;
  ALADPCMloop *loop;
} MusicTrack;

/*
 * Audio microcode requires coefficient books to be 8-byte aligned. Keep bank
 * objects separate and explicitly aligned instead of depending on incidental
 * padding inside an aggregate structure.
 */
static ALWaveTable title_wave __attribute__((aligned(8)));
static ALWaveTable game_wave __attribute__((aligned(8)));
static ALSound title_sound __attribute__((aligned(8)));
static ALSound game_sound __attribute__((aligned(8)));
static MusicBook title_book __attribute__((aligned(8))) = {
  MUSIC_TITLE_VADPCM_BOOK_ORDER,
  MUSIC_TITLE_VADPCM_BOOK_PREDICTORS,
  MUSIC_TITLE_VADPCM_BOOK_VALUES
};
static MusicBook game_book __attribute__((aligned(8))) = {
  MUSIC_GAME_VADPCM_BOOK_ORDER,
  MUSIC_GAME_VADPCM_BOOK_PREDICTORS,
  MUSIC_GAME_VADPCM_BOOK_VALUES
};
static ALADPCMloop title_loop __attribute__((aligned(8))) = {
  0, MUSIC_TITLE_VADPCM_SAMPLE_COUNT, (u32)-1,
  MUSIC_TITLE_VADPCM_LOOP_STATE
};
static ALADPCMloop game_loop __attribute__((aligned(8))) = {
  0, MUSIC_GAME_VADPCM_SAMPLE_COUNT, (u32)-1,
  MUSIC_GAME_VADPCM_LOOP_STATE
};
static ALEnvelope music_envelope __attribute__((aligned(8))) = {
  0, -1, 100000, AL_VOL_FULL, AL_VOL_FULL
};
static ALKeyMap music_keymap __attribute__((aligned(8))) = {
  0, 127, 0, 127, 60, 0
};

static MusicTrack title_track = {
  &title_wave, &title_sound, (ALADPCMBook *)&title_book, &title_loop
};
static MusicTrack game_track = {
  &game_wave, &game_sound, (ALADPCMBook *)&game_book, &game_loop
};

static ALSndId active_music = -1;
static MusicTrack *current_track = NULL;
static u16 warmup_frames = 60;

static void prepareTrack(
  MusicTrack *track,
  u8 *rom_start,
  s32 data_bytes
) {
  track->wave->base = rom_start;
  track->wave->len = data_bytes;
  track->wave->type = AL_ADPCM_WAVE;
  track->wave->flags = 1;
  track->wave->waveInfo.adpcmWave.loop = track->loop;
  track->wave->waveInfo.adpcmWave.book = track->book;

  track->sound->envelope = &music_envelope;
  track->sound->keyMap = &music_keymap;
  track->sound->wavetable = track->wave;
  track->sound->samplePan = AL_PAN_CENTER;
  track->sound->sampleVolume = AL_VOL_FULL;
  track->sound->flags = 1;
}

static void playTrack(MusicTrack *track) {
  ALSndId next_music;

  if (track == current_track) {
    return;
  }
  if (active_music != -1) {
    alSndpSetSound(&nuAuSndPlayer, active_music);
    alSndpStop(&nuAuSndPlayer);
    alSndpDeallocate(&nuAuSndPlayer, active_music);
    active_music = -1;
    current_track = NULL;
  }

  next_music = alSndpAllocate(&nuAuSndPlayer, track->sound);
  if (next_music == -1) {
    return;
  }
  active_music = next_music;
  alSndpSetSound(&nuAuSndPlayer, active_music);
  alSndpSetPitch(&nuAuSndPlayer, 1.0f);
  alSndpSetPan(&nuAuSndPlayer, AL_PAN_CENTER);
  alSndpSetVol(&nuAuSndPlayer, 22000);
  alSndpPlay(&nuAuSndPlayer);
  current_track = track;
}

void initAudio(void) {
  /*
   * Keep comfortable SDK-sized task/update buffers. The previous 0x200 Acmd
   * list sat directly before the sample buffers, so an overrun could corrupt
   * the first AI frame and wedge the RSP scheduler shared with graphics.
   */
  nuAuSynConfig.maxVVoices = 4;
  nuAuSynConfig.maxPVoices = 4;
  nuAuSynConfig.maxUpdates = 64;
  nuAuSynConfig.outputRate = MUSIC_TITLE_VADPCM_SAMPLE_RATE;
  nuAuSynConfig.fxType = AL_FX_NONE;
  nuAuSndpConfig.maxSounds = 2;
  nuAuSndpConfig.maxEvents = 16;
  nuAuDmaBufNum = 32;
  nuAuDmaBufSize = 1024;
  nuAuAcmdLen = NU_AU_CLIST_LEN;

  prepareTrack(
    &title_track,
    _musicTitleSegmentRomStart,
    MUSIC_TITLE_VADPCM_DATA_BYTES
  );
  prepareTrack(
    &game_track,
    _musicGameSegmentRomStart,
    MUSIC_GAME_VADPCM_DATA_BYTES
  );

  nuAuMgrInit((void *)NU_AU_HEAP_ADDR, NU_AU_HEAP_SIZE, &nuAuSynConfig);
  nuAuSndPlayerInit(&nuAuSndpConfig);
  nuAuPreNMIFuncSet(nuAuPreNMIProc);
}

void updateAudio(enum Screen screen) {
  /*
   * Let the scheduler, controllers, storage, and first visible frames settle
   * before the first cartridge DMA. This distinguishes a music DMA failure
   * from a boot or storage failure during real-hardware testing.
   */
  if (warmup_frames > 0) {
    warmup_frames--;
    return;
  }

  if (screen == GAME || screen == INVENTORY) {
    playTrack(&game_track);
  } else {
    playTrack(&title_track);
  }
}

#endif /* ENABLE_AUDIO */
