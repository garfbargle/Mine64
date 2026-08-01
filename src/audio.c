#include <nusys.h>

#ifdef ENABLE_AUDIO

#include <nualsgi.h>

/*
 * NU_AU_HEAP_SIZE is the SDK's 320 KiB default, sized for a game running a full
 * sequence player.  Mine64 runs four voices, 64 updates, 32 x 1 KiB streaming
 * buffers and a 2048-entry command list; the SDK's own NU_AU_HEAP_MIN_SIZE
 * formula over those numbers wants about 73 KiB.  The heap must sit directly
 * beneath the framebuffers, so shrinking it also moves its base up.
 *
 * 88 KiB rather than the 96 it was, because the audio variant had come within
 * 3 KiB of fitting for the first time and the homestead models pushed it back
 * out.  This is the conservative step: it clears the link and still leaves
 * ~15 KiB over the formula's estimate.
 *
 * It is an estimate, and an undersized audio heap fails only on hardware and
 * only as silence or corruption -- no emulator reproduces it.  So do not cut
 * this again on the strength of the formula alone: the diagnostics overlay's
 * U row reports the peak actually taken (see audioHeapPeakKiB), and that
 * measurement is what a further cut should be made against.
 */
#define MINE64_AU_HEAP_SIZE 0x16000  /* 88 KiB */
#define MINE64_AU_HEAP_ADDR (NU_GFX_FRAMEBUFFER_ADDR - MINE64_AU_HEAP_SIZE)
#include "audio.h"
#include "music_title_vadpcm.h"
#include "music_game_vadpcm.h"
#include "game_sfx.h"

extern u8 _musicTitleSegmentRomStart[];
extern u8 _musicGameSegmentRomStart[];
extern u8 _sfxPickupSegmentRomStart[];
extern u8 _sfxPunchSegmentRomStart[];
extern u8 _sfxBreakSegmentRomStart[];
extern u8 _sfxPlaceSegmentRomStart[];

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

typedef struct {
  ALWaveTable *wave;
  ALSound *sound;
  ALSndId id;
} SoundEffectTrack;

/*
 * Audio microcode requires coefficient books to be 8-byte aligned. Keep bank
 * objects separate and explicitly aligned instead of depending on incidental
 * padding inside an aggregate structure.
 */
static ALWaveTable title_wave __attribute__((aligned(8)));
static ALWaveTable game_wave __attribute__((aligned(8)));
static ALSound title_sound __attribute__((aligned(8)));
static ALSound game_sound __attribute__((aligned(8)));
static ALWaveTable sfx_pickup_wave __attribute__((aligned(8)));
static ALWaveTable sfx_punch_wave __attribute__((aligned(8)));
static ALWaveTable sfx_break_wave __attribute__((aligned(8)));
static ALWaveTable sfx_place_wave __attribute__((aligned(8)));
static ALSound sfx_pickup_sound __attribute__((aligned(8)));
static ALSound sfx_punch_sound __attribute__((aligned(8)));
static ALSound sfx_break_sound __attribute__((aligned(8)));
static ALSound sfx_place_sound __attribute__((aligned(8)));
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
static ALEnvelope sfx_envelope __attribute__((aligned(8))) = {
  0, 0, 0, AL_VOL_FULL, AL_VOL_FULL
};
static ALKeyMap sfx_keymap __attribute__((aligned(8))) = {
  0, 127, 0, 127, 60, 0
};

static MusicTrack title_track = {
  &title_wave, &title_sound, (ALADPCMBook *)&title_book, &title_loop
};
static MusicTrack game_track = {
  &game_wave, &game_sound, (ALADPCMBook *)&game_book, &game_loop
};
static SoundEffectTrack sfx_tracks[] = {
  { &sfx_pickup_wave, &sfx_pickup_sound, -1 },
  { &sfx_punch_wave, &sfx_punch_sound, -1 },
  { &sfx_break_wave, &sfx_break_sound, -1 },
  { &sfx_place_wave, &sfx_place_sound, -1 }
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

static void prepareEffect(
  SoundEffectTrack *track,
  u8 *rom_start,
  s32 data_bytes
) {
  track->wave->base = rom_start;
  track->wave->len = data_bytes;
  track->wave->type = AL_RAW16_WAVE;
  track->wave->flags = 0;
  track->wave->waveInfo.rawWave.loop = NULL;

  track->sound->envelope = &sfx_envelope;
  track->sound->keyMap = &sfx_keymap;
  track->sound->wavetable = track->wave;
  track->sound->samplePan = AL_PAN_CENTER;
  track->sound->sampleVolume = AL_VOL_FULL;
  track->sound->flags = 0;
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
  /* One music voice and up to three brief effects can coexist. */
  nuAuSndpConfig.maxSounds = 5;
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
  prepareEffect(&sfx_tracks[SOUND_PICKUP], _sfxPickupSegmentRomStart,
    SFX_PICKUP_DATA_BYTES);
  prepareEffect(&sfx_tracks[SOUND_PUNCH], _sfxPunchSegmentRomStart,
    SFX_PUNCH_DATA_BYTES);
  prepareEffect(&sfx_tracks[SOUND_BREAK], _sfxBreakSegmentRomStart,
    SFX_BREAK_DATA_BYTES);
  prepareEffect(&sfx_tracks[SOUND_PLACE], _sfxPlaceSegmentRomStart,
    SFX_PLACE_DATA_BYTES);

  nuAuMgrInit((void *)MINE64_AU_HEAP_ADDR, MINE64_AU_HEAP_SIZE, &nuAuSynConfig);
  nuAuSndPlayerInit(&nuAuSndpConfig);
  nuAuPreNMIFuncSet(nuAuPreNMIProc);
}

u32 audioHeapPeakKiB(void) {
  /* alHeap never frees, so used is monotonic and this is already the peak. */
  return (u32) nuAuHeapGetUsed() / 1024;
}

void playSound(enum SoundEffect effect) {
  SoundEffectTrack *track;

  if (effect < SOUND_PICKUP || effect > SOUND_PLACE) {
    return;
  }
  track = &sfx_tracks[effect];

  /* Reuse the effect's slot. This keeps rapid punches responsive without
     leaking sound-player allocations after a clip has naturally ended. */
  if (track->id != -1) {
    alSndpSetSound(&nuAuSndPlayer, track->id);
    alSndpStop(&nuAuSndPlayer);
    alSndpDeallocate(&nuAuSndPlayer, track->id);
    track->id = -1;
  }
  track->id = alSndpAllocate(&nuAuSndPlayer, track->sound);
  if (track->id == -1) {
    return;
  }
  alSndpSetSound(&nuAuSndPlayer, track->id);
  alSndpSetPitch(&nuAuSndPlayer, 1.0f);
  alSndpSetPan(&nuAuSndPlayer, AL_PAN_CENTER);
  alSndpSetVol(&nuAuSndPlayer, 21000);
  alSndpPlay(&nuAuSndPlayer);
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
