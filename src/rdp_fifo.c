/*
 * Mine64's own RDP command FIFO, replacing NuSystem's.
 *
 * The FIFO is the ring the RSP writes RDP commands into and the RDP drains.
 * NuSystem sizes it at NU_GFX_RDP_OUTPUTBUFF_SIZE (128 KiB) -- an SDK default,
 * not a number this game ever chose, and the fourth largest object in the
 * image.
 *
 * It can be replaced without rebuilding libnusys.  `nuRDPOutputBuf` is the
 * only symbol in its archive member (nurdpoutput.o), so defining it here means
 * the linker resolves nusys's reference against this array and never pulls the
 * member in at all.
 *
 * The size must be handed over explicitly.  nuGfxInit calls
 * nuGfxSetUcodeFifo(nuRDPOutputBuf, NU_GFX_RDP_OUTPUTBUFF_SIZE) -- it passes
 * the SDK constant, not sizeof, so a smaller array alone would leave the RSP
 * believing it has 128 KiB to write into.  mine64SetRDPFifo re-registers the
 * real size and must be called immediately after nuGfxInit.
 *
 * With the fifo microcode a short FIFO costs throughput, not correctness: the
 * RSP stalls until the RDP drains rather than overrunning anything.  16-byte
 * alignment is required by the hardware.
 */

#include <nusys.h>

#include "graphics.h"

u8 nuRDPOutputBuf[MINE64_RDP_FIFO_SIZE] __attribute__((aligned(16)));

void mine64SetRDPFifo(void) {
  nuGfxSetUcodeFifo(nuRDPOutputBuf, sizeof(nuRDPOutputBuf));
}
