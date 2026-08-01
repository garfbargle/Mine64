#!/usr/bin/env python3
"""Guard the link against growing into NuSystem's fixed RDRAM reservations.

NuSystem does not allocate its framebuffers or its audio heap from the linked
image.  It pins them at absolute addresses at the top of a 4 MiB RDRAM:

    Z buffer     NU_GFX_ZBUFFER_ADDR      0x80000400 .. +320*240*2
    audio heap   MINE64_AU_HEAP_ADDR      framebuffers - AUDIO_HEAP_SIZE below
    framebuffers NU_GFX_FRAMEBUFFER_ADDR  0x80400000 - 320*240*2*3

Nothing at link time notices when BSS runs past those, and the two 1 MiB
column display list arenas dominate BSS.  An overrun does not fail the build;
it corrupts audio or video at runtime on hardware.  Check it explicitly.
"""

import struct
import sys

RDRAM_END = 0x80400000
FRAMEBUFFER_BYTES = 320 * 240 * 2
FRAMEBUFFER_ADDR = RDRAM_END - FRAMEBUFFER_BYTES * 3
# Must track MINE64_AU_HEAP_SIZE in src/audio.c -- audio.c passes the size to
# nuAuMgrInit itself, so this is not an SDK constant to look up but a number
# the two files have to agree on.
AUDIO_HEAP_SIZE = 0x16000
AUDIO_HEAP_ADDR = FRAMEBUFFER_ADDR - AUDIO_HEAP_SIZE

SHF_ALLOC = 0x2

# Sections of the BOOT/OBJECT spec segment, which is what is resident in RDRAM
# for the whole run.  The RAW spec segments (the VADPCM music and SFX banks)
# are ROM payloads read on demand; spicy still assigns them link addresses, so
# counting those would badly overstate the footprint.  Add any new OBJECT
# segment here.
RESIDENT_SECTIONS = ("..generatedStartEntry", "..program")


def resident(name):
    return name == RESIDENT_SECTIONS[0] or name.startswith(RESIDENT_SECTIONS[1])


def image_end(path):
    """Return the highest RDRAM address occupied by the resident image."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("%s: not an ELF file" % path)
    endian = ">" if data[5] == 2 else "<"
    sh_off, = struct.unpack(endian + "I", data[0x20:0x24])
    sh_entsize, sh_num = struct.unpack(endian + "HH", data[0x2E:0x32])
    str_ndx, = struct.unpack(endian + "H", data[0x32:0x34])

    def section(i):
        off = sh_off + i * sh_entsize
        return struct.unpack(endian + "6I", data[off:off + 24])

    _, _, _, _, str_off, str_size = section(str_ndx)
    strtab = data[str_off:str_off + str_size]

    end = 0
    for i in range(sh_num):
        name_off, _, flags, addr, _, size = section(i)
        name = strtab[name_off:strtab.index(b"\0", name_off)].decode()
        if flags & SHF_ALLOC and addr >= 0x80000000 and resident(name):
            end = max(end, addr + size)
    if end == 0:
        raise SystemExit("%s: found no resident sections" % path)
    return end


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: check_ram.py <elf> [--audio]")
    path = argv[1]
    audio = "--audio" in argv[2:]

    limit = AUDIO_HEAP_ADDR if audio else FRAMEBUFFER_ADDR
    limit_name = "audio heap" if audio else "framebuffers"
    end = image_end(path)
    slack = limit - end

    print("%s: image ends at 0x%08X, %s begin at 0x%08X (%d KiB free)"
          % (path, end, limit_name, limit, slack // 1024))

    if slack < 0:
        raise SystemExit(
            "ERROR: image overruns the %s by %d KiB.  Reduce DISPLAY_LIST_SIZE,"
            " FRAME_DISPLAY_LIST_SIZE, or world dimensions."
            % (limit_name, -slack // 1024))
    if slack < 64 * 1024:
        print("WARNING: under 64 KiB of headroom before the %s." % limit_name,
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
