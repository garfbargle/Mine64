# Modern N64 SDK-compatible build for Mine64 v0.3.
#
# ROOT defaults to the compatibility tree supplied by the build container.
# Set ROOT to a locally installed Modern N64 SDK root when building elsewhere.
ROOT ?= /etc/n64
N64KITDIR ?= $(ROOT)/usr

include $(ROOT)/usr/include/make/PRdefs

NUSYSINCDIR := $(N64KITDIR)/include/nusys
NUSYSLIBDIR := $(N64KITDIR)/lib/nusys

SRCDIR := src
INCDIR := include
ASSDIR := assets
OBJDIR := build
TARGET := mine64

LIB := $(ROOT)/usr/lib
MAKEMASK ?= makemask
SPICY_LD ?= $(CURDIR)/toolchain/spicy-ld.sh
MAKEROM := spicy --toolchain-prefix=mips-n64- --ld_command=$(SPICY_LD)

LCINCS := -I. -I$(NUSYSINCDIR) -I$(ROOT)/usr/include/PR -I$(INCDIR) -I$(INCDIR)/ff -I$(ASSDIR) -I$(ROOT)/usr/include
LCOPTS := -mips3 -mgp32 -mfp32 -funsigned-char -fno-common -Wall -Wextra \
	-Wno-missing-braces -Wno-unused-parameter \
	-D_LANGUAGE_C -D_ULTRA64 -D__EXTENSIONS__

DEBUG ?= 0
AUDIO ?= 0
ifeq ($(DEBUG),1)
SDK_VARIANT := _d
BUILD_VARIANT := debug
LCDEFS := -DNU_DEBUG -DF3DEX_GBI_2
OPTIMIZER := -O0 -g3
else
SDK_VARIANT :=
BUILD_VARIANT := release
LCDEFS := -DF3DEX_GBI_2 -DNDEBUG
OPTIMIZER := -O2 -g
endif

SPEC := spec
AUDIO_LIB :=
ifeq ($(AUDIO),1)
TARGET := mine64-audio
BUILD_VARIANT := $(BUILD_VARIANT)-audio
LCDEFS += -DENABLE_AUDIO
AUDIO_LIB := -lnualsgi$(SDK_VARIANT)
SPEC := spec.audio
endif

LDFLAGS := $(MKDEPOPT) -L$(LIB) -L$(NUSYSLIBDIR) -lnusys$(SDK_VARIANT) \
	$(AUDIO_LIB) -lultra$(SDK_VARIANT) -lcart

OBJECT_DIR := $(OBJDIR)/$(BUILD_VARIANT)
APP := $(OBJDIR)/$(TARGET).out
ROM := $(OBJDIR)/$(TARGET).n64
CODEFILES := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/ff/*.c)
APP_OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJECT_DIR)/%.o,$(CODEFILES))
CODEOBJECTS := $(APP_OBJECTS) $(NUSYSLIBDIR)/nusys$(SDK_VARIANT).o
DEPFILES := $(APP_OBJECTS:.o=.d)
ifeq ($(AUDIO),1)
CODESEGMENT := $(OBJDIR)/audio-codesegment.o
else
CODESEGMENT := $(OBJDIR)/codesegment.o
endif
BASE_ASSETS := $(ASSDIR)/texture_data.h $(ASSDIR)/font.h
CUSTOM_TEXTURE_SOURCE := $(wildcard art/custom-textures.png)

# Encoded audio is versioned with the source tree.  The original WAV masters
# are only needed for the explicit `make music MUSIC_SOURCE_DIR=/path` import.
AUDIO_ASSET_DIR := $(ASSDIR)/audio
AUDIO_IMPORT_DIR := $(OBJDIR)/audio-import
MUSIC_SOURCE_DIR ?=
# 12 kHz is the best quality/ROM-size balance for Mine64's background music.
# When raising MUSIC_RATE, raise MUSIC_LOWPASS accordingly (for example 7000
# at 16 kHz or 10000 at 22.05 kHz).
MUSIC_RATE ?= 12000
MUSIC_LOWPASS ?= 5200
MUSIC_EFFECTS ?= highpass 30 lowpass $(MUSIC_LOWPASS) gain -n -3
MUSIC_TITLE_PCM := $(AUDIO_IMPORT_DIR)/music-title-$(MUSIC_RATE)-mono.wav
MUSIC_GAME_PCM := $(AUDIO_IMPORT_DIR)/music-game-$(MUSIC_RATE)-mono.wav
MUSIC_TITLE_AIFC := $(AUDIO_IMPORT_DIR)/music-title.vadpcm.aifc
MUSIC_GAME_AIFC := $(AUDIO_IMPORT_DIR)/music-game.vadpcm.aifc
MUSIC_TITLE_BIN := $(AUDIO_ASSET_DIR)/music-title.vadpcm.bin
MUSIC_GAME_BIN := $(AUDIO_ASSET_DIR)/music-game.vadpcm.bin
MUSIC_TITLE_HEADER := $(ASSDIR)/music_title_vadpcm.h
MUSIC_GAME_HEADER := $(ASSDIR)/music_game_vadpcm.h
MUSIC_ASSETS := $(MUSIC_TITLE_BIN) $(MUSIC_GAME_BIN) $(MUSIC_TITLE_HEADER) $(MUSIC_GAME_HEADER)
SFX_DIR := $(AUDIO_ASSET_DIR)/sfx
SFX_HEADER := $(ASSDIR)/game_sfx.h
SFX_ASSETS := $(SFX_HEADER) $(SFX_DIR)/pickup.pcm.bin $(SFX_DIR)/punch.pcm.bin \
	$(SFX_DIR)/break.pcm.bin $(SFX_DIR)/place.pcm.bin
ifeq ($(AUDIO),1)
ASSETS := $(BASE_ASSETS) $(MUSIC_TITLE_HEADER) $(MUSIC_GAME_HEADER) $(SFX_HEADER)
else
ASSETS := $(BASE_ASSETS)
endif
SOX ?= sox
VADPCM ?= vadpcm

.DEFAULT_GOAL := default
.PHONY: default audio clean music sfx FORCE_CODESEGMENT FORCE_ROM

default: $(ROM)

audio:
	$(MAKE) AUDIO=1

# Run this inside the project Docker image when replacing the versioned music
# payloads. The regular audio build never reads artist masters from Downloads.
music:
	@test -n "$(MUSIC_SOURCE_DIR)" || (echo "Set MUSIC_SOURCE_DIR to the directory holding the WAV masters." >&2; exit 2)
	@mkdir -p $(AUDIO_IMPORT_DIR) $(AUDIO_ASSET_DIR)
	$(SOX) "$(MUSIC_SOURCE_DIR)/Softstone Sunset.wav" -r $(MUSIC_RATE) -c 1 -b 16 -e signed-integer $(MUSIC_TITLE_PCM) remix - $(MUSIC_EFFECTS)
	$(VADPCM) encode --predictors 4 $(MUSIC_TITLE_PCM) $(MUSIC_TITLE_AIFC)
	python3 tools/audio_codegen.py $(MUSIC_TITLE_AIFC) $(MUSIC_TITLE_BIN) $(MUSIC_TITLE_HEADER) MUSIC_TITLE
	$(SOX) "$(MUSIC_SOURCE_DIR)/Still Exploring.wav" -r $(MUSIC_RATE) -c 1 -b 16 -e signed-integer $(MUSIC_GAME_PCM) remix - $(MUSIC_EFFECTS)
	$(VADPCM) encode --predictors 4 $(MUSIC_GAME_PCM) $(MUSIC_GAME_AIFC)
	python3 tools/audio_codegen.py $(MUSIC_GAME_AIFC) $(MUSIC_GAME_BIN) $(MUSIC_GAME_HEADER) MUSIC_GAME

sfx:
	python3 tools/generate_sfx.py

# These are checked-in binary ROM inputs. Explicitly declaring them prevents
# make's built-in .o-to-file rule from mistaking spicy's assembler sidecars for
# an instruction to rebuild the source payload.
$(MUSIC_ASSETS) $(SFX_ASSETS):
	@test -f $@

$(BASE_ASSETS): generate_assets.py tools/import_textures.py $(CUSTOM_TEXTURE_SOURCE)
	python3 generate_assets.py
	@if [ -n "$(CUSTOM_TEXTURE_SOURCE)" ]; then python3 tools/import_textures.py $(CUSTOM_TEXTURE_SOURCE); fi

ifeq ($(AUDIO),1)
$(ROM): $(MUSIC_ASSETS) $(SFX_ASSETS)
endif

$(OBJECT_DIR)/%.o: $(SRCDIR)/%.c $(ASSETS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) $< -o $@

-include $(DEPFILES)

$(CODESEGMENT): FORCE_CODESEGMENT $(CODEOBJECTS) Makefile
	$(LD) -o $@ -r $(CODEOBJECTS) $(LDFLAGS)

# Recreate the shared output when switching between cached release/debug
# object trees, even if its timestamp is newer than the selected code segment.
$(ROM): FORCE_ROM $(CODESEGMENT) $(SPEC)
	$(MAKEROM) $(SPEC) -I$(NUSYSINCDIR) -r $@ -e $(APP)
	$(MAKEMASK) $@

FORCE_ROM:

FORCE_CODESEGMENT:

clean:
	rm -rf $(OBJDIR)
