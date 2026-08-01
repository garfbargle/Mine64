#!/usr/bin/env python3
"""Re-measure RDRAM use from the linked ELFs and refresh docs/ram-report.html.

Companion to check_ram.py, which answers only "does it fit".  This one answers
"where did it go": it walks the section headers and the symbol table of both
linked images, buckets every resident symbol, and rewrites the `const DATA`
line the treemap in docs/ram-report.html reads.

    docker run ... make && docker run ... make AUDIO=1     # both ELFs first
    python3 tools/ram_report.py

Like check_ram.py it parses the ELF by hand, so it needs no toolchain.  The
figures it prints are the ones docs/ram-budget.md quotes; re-run both together.
"""

import json
import re
import struct
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

RDRAM_END = 0x80400000
FRAMEBUFFER_BYTES = 320 * 240 * 2
FRAMEBUFFER_ADDR = RDRAM_END - FRAMEBUFFER_BYTES * 3
ZBUFFER_ADDR = 0x80000400
ZBUFFER_BYTES = 320 * 240 * 2
# Must track MINE64_AU_HEAP_SIZE in src/audio.c, exactly as check_ram.py does.
AUDIO_HEAP_SIZE = 0x11800  # 70 KiB, against a measured 64 KiB peak
AUDIO_HEAP_ADDR = FRAMEBUFFER_ADDR - AUDIO_HEAP_SIZE

SHF_ALLOC = 0x2
SHT_SYMTAB = 2
STT_OBJECT, STT_FUNC = 1, 2

# Only the BOOT/OBJECT spec segment is resident; the RAW segments (VADPCM music
# and SFX) are ROM payloads that spicy still gives link addresses.  Same rule
# check_ram.py applies -- keep the two in step.
TEXT_SECTION = "..program"
BSS_SECTION = "..program.bss"


# ---------------------------------------------------------------- ELF reading

def read_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("%s: not an ELF file" % path)
    endian = ">" if data[5] == 2 else "<"
    sh_off, = struct.unpack(endian + "I", data[0x20:0x24])
    sh_entsize, sh_num = struct.unpack(endian + "HH", data[0x2E:0x32])
    str_ndx, = struct.unpack(endian + "H", data[0x32:0x34])

    def section(i):
        off = sh_off + i * sh_entsize
        keys = ("name", "type", "flags", "addr", "offset", "size", "link",
                "info", "align", "entsize")
        return dict(zip(keys, struct.unpack(endian + "10I", data[off:off + 40])))

    secs = [section(i) for i in range(sh_num)]
    shstr = secs[str_ndx]
    blob = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
    for s in secs:
        s["sname"] = blob[s["name"]:blob.index(b"\0", s["name"])].decode()

    syms, seen = [], set()
    for s in secs:
        if s["type"] != SHT_SYMTAB:
            continue
        strtab = secs[s["link"]]
        st = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
        for i in range(s["size"] // s["entsize"]):
            off = s["offset"] + i * s["entsize"]
            nm, value, size, info, _other, shndx = struct.unpack(
                endian + "IIIBBH", data[off:off + 16])
            kind = info & 0xF
            if kind not in (STT_OBJECT, STT_FUNC) or not size:
                continue
            if value < 0x80000000 or (value, size) in seen:
                continue
            seen.add((value, size))
            syms.append(dict(
                name=st[nm:st.index(b"\0", nm)].decode(), addr=value,
                size=size, func=(kind == STT_FUNC),
                sec=secs[shndx]["sname"] if shndx < len(secs) else "?"))

    alloc = {s["sname"]: s for s in secs
             if s["flags"] & SHF_ALLOC and s["addr"] >= 0x80000000}
    return alloc, syms


# ------------------------------------------------------------------- bucketing

def has_prefix(*prefixes):
    return lambda n: n.startswith(prefixes)


def is_one_of(*names):
    return lambda n: n in names


# Ordered; first match wins.  The last bucket is the catch-all, which is why
# it is named for what actually lands in it rather than just for the stacks.
BSS_GROUPS = [
    ("Mesh arena", has_prefix("mesh_")),
    ("Home store", has_prefix("home")),
    ("Per-slot render tables", is_one_of(
        "column_starts", "staged_starts", "c_models", "visible_columns",
        "column_meshed", "column_mesh_lod", "staged_meshed", "staged_mesh_lod",
        "dirty_columns")),
    ("Frame display lists", is_one_of(
        "frame_display_lists", "column_quads", "column_baked")),
    ("Terrain window", lambda n: n.startswith("window_") or n in (
        "details", "trees", "detail_count", "tree_count")),
    ("Entity matrices", lambda n: n.startswith((
        "mob_", "creature_", "humanoid_", "detail_matrix", "dropped_item_",
        "falling_tree_", "shadow_", "water_top_", "special_flash_",
        "first_person_")) ),
    ("NuSystem / RSP", has_prefix("nu")),
    ("Threads, debug & misc", lambda n: True),
]

# Notes that ride along on a symbol wherever it lands.  Prose only; nothing
# here affects a number.
NOTES = {
    "mesh_arena": "147,456 Gfx of column meshes, first-fit with relocation "
                  "defrag. Still the largest single object in the game.",
    "mesh_blocks": "2,048 allocation records: every live column plus every "
                   "staged replacement.",
    "window_blocks": "1,024 column slots x 1 KiB. The stream radius can fill "
                     "625 of them.",
    "home_blocks": "The 112x32x112 save extent kept whole, so building is "
                   "never rationed.",
    "frame_display_lists": "2 x 6,656 Gfx. World, targeting and HUD share one "
                           "RSP task.",
    "column_starts": "Gfx* per texture bank per slot, for the generation on "
                     "screen.",
    "staged_starts": "The same table again, for a world build in flight.",
    "c_models": "One 64-byte Mtx per slot holding nothing but a translation.",
    "nuRDPOutputBuf": "Mine64's own RDP FIFO, half the SDK's 128 KiB. "
                      "src/rdp_fifo.c.",
    "rmonIOStack": "libultra's remote debug monitor. Welded to the exception "
                   "handler; not removable from here.",
}

TOP_PER_GROUP = 8


def roll_up(members, top=TOP_PER_GROUP):
    """Name the biggest few; fold the tail into one 'N smaller' box."""
    members = sorted(members, key=lambda s: -s["size"])
    out = [dict(n=s["name"], v=s["size"], d=NOTES.get(s["name"], ""))
           for s in members[:top]]
    tail = members[top:]
    if tail:
        out.append(dict(n="%d smaller" % len(tail),
                        v=sum(s["size"] for s in tail), f=1))
    return out


def build_tree(path, audio):
    alloc, syms = read_elf(path)
    text_sec, bss_sec = alloc[TEXT_SECTION], alloc[BSS_SECTION]
    end = max(s["addr"] + s["size"] for s in alloc.values()
              if s["sname"] in (TEXT_SECTION, BSS_SECTION,
                                "..generatedStartEntry"))
    limit = AUDIO_HEAP_ADDR if audio else FRAMEBUFFER_ADDR

    text_syms = [s for s in syms if s["sec"] == TEXT_SECTION]
    bss_syms = [s for s in syms if s["sec"] == BSS_SECTION]

    ucode = sum(s["size"] for s in text_syms
                if s["name"].startswith(("gsp", "asp")))
    code = sum(s["size"] for s in text_syms
               if s["func"] and not s["name"].startswith(("gsp", "asp")))
    rodata = sum(s["size"] for s in text_syms
                 if not s["func"] and not s["name"].startswith(("gsp", "asp")))

    groups, leftover = [], list(bss_syms)
    for name, match in BSS_GROUPS:
        mine = [s for s in leftover if match(s["name"])]
        leftover = [s for s in leftover if not match(s["name"])]
        if mine:
            groups.append(dict(n=name, v=sum(s["size"] for s in mine),
                               c=roll_up(mine)))
    groups.sort(key=lambda g: -g["v"])
    named = sum(g["v"] for g in groups)
    if bss_sec["size"] > named:
        groups.append(dict(n="padding", v=bss_sec["size"] - named, f=1))

    children = [
        dict(n="Z buffer", v=ZBUFFER_BYTES, k="resv",
             d="NU_GFX_ZBUFFER_ADDR. The image starts immediately above it, "
               "so none of it is wasted."),
        dict(n="Code + data", v=text_sec["size"], k="code", c=[
            dict(n="Game code", v=code),
            dict(n="Microcode", v=ucode,
                 d="One graphics microcode now, down from six. 29 KiB "
                   "reclaimed."),
            dict(n="Read-only data", v=rodata),
            dict(n="padding", v=text_sec["size"] - code - ucode - rodata, f=1),
        ]),
        dict(n="BSS", v=bss_sec["size"], k="bss", c=groups),
    ]
    slack = limit - end
    if slack > 0:
        children.append(dict(
            n="Free", v=slack, k="free",
            d="Headroom before the audio heap." if audio
              else "Headroom before the framebuffers."))
    else:
        children.append(dict(
            n="Overrun", v=-slack, k="crit",
            d="The image runs %d bytes past the audio heap. Two owners write "
              "the same RDRAM." % -slack if audio else
              "The image runs %d bytes past the framebuffers." % -slack))
    if audio:
        children.append(dict(
            n="Audio heap", v=AUDIO_HEAP_SIZE, k="warn",
            d="%d KiB, sized from the SDK's own formula for four voices. The "
              "U diagnostic row reports what is really used."
              % (AUDIO_HEAP_SIZE // 1024)))
    children.append(dict(
        n="Framebuffers", v=FRAMEBUFFER_BYTES * 3, k="resv",
        d="Three 320x240 16bpp buffers, pinned at the top of RDRAM."))

    return dict(c=children, end=end, limit=limit, slack=slack,
                bss=bss_sec["size"], text=text_sec["size"],
                fns=sum(1 for s in text_syms if s["func"]),
                objs=len(bss_syms))


# ------------------------------------------------------------------- reporting

def summarise(tag, tree):
    print("%s: image ends at 0x%08X, limit 0x%08X -- %s"
          % (tag, tree["end"], tree["limit"],
             "%d KiB free" % (tree["slack"] // 1024) if tree["slack"] > 0
             else "OVER by %d bytes" % -tree["slack"]))
    print("  code + data %8d B (%6.1f KiB)" % (tree["text"], tree["text"] / 1024))
    print("  BSS         %8d B (%6.2f MiB)" % (tree["bss"], tree["bss"] / 1048576))
    for g in [c for c in tree["c"] if c["n"] == "BSS"][0]["c"]:
        print("    %-24s %8d B (%6.1f KiB) %5.1f%%"
              % (g["n"], g["v"], g["v"] / 1024, 100 * g["v"] / tree["bss"]))


def main(argv):
    plain = os.path.join(ROOT, "mine64.out")
    audio = os.path.join(ROOT, "mine64-audio.out")
    report = os.path.join(ROOT, "docs", "ram-report.html")
    missing = [p for p in (plain, audio) if not os.path.exists(p)]
    if missing:
        raise SystemExit("missing linked ELF(s): %s\nBuild `make` and "
                         "`make AUDIO=1` first." % ", ".join(missing))

    data = dict(nonaudio=build_tree(plain, False),
                audio=build_tree(audio, True))
    summarise("make", data["nonaudio"])
    print()
    summarise("make audio", data["audio"])

    if "--print" in argv:
        return 0
    html = open(report).read()
    line = "const DATA = %s;" % json.dumps(data, separators=(",", ":"))
    html, n = re.subn(r"^const DATA = .*;$", lambda _: line, html,
                      count=1, flags=re.M)
    if n != 1:
        raise SystemExit("%s: could not find the `const DATA` line" % report)
    open(report, "w").write(html)
    print("\nrewrote %s" % report)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
