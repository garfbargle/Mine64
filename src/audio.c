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
 * 70 KiB is no longer the formula's estimate.  It was 96, then 88, both of them
 * guesses with a margin bolted on; the U row was then read on hardware in game
 * and the peak is 64 KiB.  So this is 6 KiB over a measurement rather than
 * ~15 KiB over an estimate, and it is the smaller number that is better
 * founded.  The 18 KiB it returns is what puts the audio variant back under
 * NuSystem's ceiling with room to work in.
 *
 * That reading is a ceiling and not a sample: alHeap never frees, and every
 * allocation of consequence happens in initAudio below -- nuAuMgrInit takes
 * the voices, the 32 x 1 KiB DMA buffers and the command list, and
 * nuAuSndPlayerInit takes maxSounds worth of sound state.  alSndpAllocate at
 * play time hands out one of those pre-allocated slots.  The heap is therefore
 * at its high-water mark before the first note plays, which is why a single
 * in-game reading settles it.
 *
 * An undersized audio heap still fails only on hardware and only as silence or
 * corruption -- no emulator reproduces it.  Anything that changes maxVVoices,
 * maxPVoices, maxUpdates, nuAuDmaBufNum/Size, nuAuAcmdLen or maxSounds moves
 * the ceiling, so re-read the U row (see audioHeapPeakKiB) after touching any
 * of them rather than trusting this number to hold.
 */
#define MINE64_AU_HEAP_SIZE 0x11800  /* 70 KiB; measured peak is 64 KiB */
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

/*
 * The step tables.  The top entry of each is the level the game shipped at,
 * so a console left alone sounds exactly as it always did; the rest are not a
 * linear ramp, because loudness is not -- halving the amplitude is nothing
 * like halving the perceived volume, and evenly spaced numbers make the lower
 * pips indistinguishable from each other.
 */
static const s32 music_volume_table[AUDIO_VOLUME_STEPS] = {
  0, 5000, 9500, 15000, 22000
};
static const s32 sound_volume_table[AUDIO_VOLUME_STEPS] = {
  0, 4800, 9000, 14500, 21000
};
static u8 music_volume_step = AUDIO_VOLUME_DEFAULT;
static u8 sound_volume_step = AUDIO_VOLUME_DEFAULT;

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
  alSndpSetVol(&nuAuSndPlayer, music_volume_table[music_volume_step]);
  alSndpPlay(&nuAuSndPlayer);
  current_track = track;
}

void setMusicVolume(u8 step) {
  music_volume_step = step < AUDIO_VOLUME_STEPS ? step : AUDIO_VOLUME_DEFAULT;
  if (active_music == -1) {
    return;
  }
  /* The player is a single cursor: point it at the music voice before setting
     the level, or the number lands on whichever effect was played last. */
  alSndpSetSound(&nuAuSndPlayer, active_music);
  alSndpSetVol(&nuAuSndPlayer, music_volume_table[music_volume_step]);
}

void setSoundVolume(u8 step) {
  /* Effects are allocated per shot, so there is nothing live to retune; the
     next punch reads the new number. */
  sound_volume_step = step < AUDIO_VOLUME_STEPS ? step : AUDIO_VOLUME_DEFAULT;
}

u8 musicVolume(void) {
  return music_volume_step;
}

u8 soundVolume(void) {
  return sound_volume_step;
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
  /* Silence costs a voice allocation and a DMA otherwise, several times a
     second while mining. */
  if (sound_volume_step == 0) {
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
  alSndpSetVol(&nuAuSndPlayer, sound_volume_table[sound_volume_step]);
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
