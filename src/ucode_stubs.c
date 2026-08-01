/*
 * Placeholders for the graphics microcodes Mine64 does not run.
 *
 * NuSystem's nuGfxInitEX2 builds a static table of all six microcodes the SDK
 * ships and hands it to nuGfxSetUcode.  nuGfxTaskStart then indexes that table
 * by its ucode argument -- it is a pointer table, and an entry is only ever
 * dereferenced by a task that asks for it.
 *
 * Mine64 asks for exactly one.  draw() in graphics.c holds the only
 * nuGfxTaskStart call in the tree and always passes NU_GFX_UCODE_F3DEX, which
 * is index 0, F3DEX2; nuGfxInitEX2's own one-off RDP-state task uses the same
 * index.  The world, the targeting wireframe and the HUD were three tasks on
 * three microcodes -- F3DEX, L3DEX, S2DEX -- until they were collapsed into a
 * single F3DEX2 task.  That was the right change and it stays; these stubs are
 * the leftover it did not clean up.  L3DEX2 and S2DEX2 have had no caller
 * since, and the three Rej/NoN variants never had one.
 *
 * So five microcodes sat in RDRAM for the whole run and never executed -- 29
 * KiB on a machine that had 35 KiB of headroom.  They are no longer linked
 * (see `spec` and `spec.audio`); these definitions exist only so the table in
 * nusys.o still resolves.  They are const so they land in rodata rather than
 * costing BSS.
 *
 * If a second microcode is ever wanted again -- a sprite pass on S2DEX2, say --
 * put its object back in both spec files and delete the matching pair here.
 * Nothing else has to change.  Getting it wrong fails the link rather than the
 * console: an unresolved gsp* symbol is a link error, and asking
 * nuGfxTaskStart for a stubbed index would be a visible dead frame, not a
 * silent corruption.
 */

typedef long long int UcodeWord;

#define MINE64_UNUSED_UCODE(name) \
  const UcodeWord name##TextStart[1] = { 0 }; \
  const UcodeWord name##DataStart[1] = { 0 }

MINE64_UNUSED_UCODE(gspF3DEX2_NoN_fifo);
MINE64_UNUSED_UCODE(gspF3DEX2_Rej_fifo);
MINE64_UNUSED_UCODE(gspF3DLX2_Rej_fifo);
MINE64_UNUSED_UCODE(gspL3DEX2_fifo);
MINE64_UNUSED_UCODE(gspS2DEX2_fifo);
