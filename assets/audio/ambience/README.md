# Ambience assets

These are repository-local 12 kHz mono N64 VADPCM assets, imported from the
private WAV masters in Downloads. They are deliberately **not wired into the
ROM spec or runtime yet**; this directory is the prepared asset bank for the
next audio-integration pass.

| Asset | Intended use | Duration | VADPCM payload |
| --- | --- | ---: | ---: |
| `forest_loop` | forest/overworld bed | 11.72 s | 79,110 B |
| `lava_loop` | proximity layer for lava | 11.32 s | 76,410 B |
| `underwater_loop` | submerged current layer | 7.72 s | 52,110 B |
| `rain_loop` | weather bed | 10.00 s | 67,500 B |
| `insects_loop` | nighttime bed | 10.80 s | 72,900 B |
| `bird_oneshot` | sparse daytime one-shot | 12.92 s | 87,210 B |
| `wind_loop` | exposed/high-altitude bed | 9.48 s | 63,990 B |

The filename suffix is the intended playback mode. For every asset name there
are three files:

- `<name>.vadpcm.bin` — ROM payload.
- `<name>_vadpcm.h` — sample count, codebook, and loop-state macros for the
  N64 audio player.
- `<name>.json` — human-readable import manifest.

All assets were downmixed to mono, resampled to 12 kHz, high-passed at 30 Hz,
low-passed at 5.2 kHz, peak-normalized to -3 dBFS, and encoded with four
VADPCM predictors. The loop metadata spans each complete clip. Treat that as
an implementation starting point, not a guarantee of a seamless loop; audition
the head/tail during integration. `bird_oneshot` should normally be scheduled
as an occasional one-shot rather than looped.
