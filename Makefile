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

LDFLAGS := $(MKDEPOPT) -L$(LIB) -L$(NUSYSLIBDIR) -lnusys$(SDK_VARIANT) \
	-lultra$(SDK_VARIANT) -lcart

OBJECT_DIR := $(OBJDIR)/$(BUILD_VARIANT)
APP := $(OBJDIR)/$(TARGET).out
ROM := $(OBJDIR)/$(TARGET).n64
CODEFILES := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/ff/*.c)
CODEOBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJECT_DIR)/%.o,$(CODEFILES)) $(NUSYSLIBDIR)/nusys$(SDK_VARIANT).o
CODESEGMENT := $(OBJDIR)/codesegment.o
BASE_ASSETS := $(ASSDIR)/texture_data.h $(ASSDIR)/font.h
CUSTOM_TEXTURE_SOURCE := $(wildcard art/custom-textures.png)

# The original WAVs are local artist assets, while these generated files are
# compact ROM inputs. Override MUSIC_SOURCE_DIR when building elsewhere.
AUDIO_BUILD_DIR := $(OBJDIR)/audio
MUSIC_SOURCE_DIR ?= /Users/codi/Downloads
MUSIC_TITLE_SOURCE ?= $(MUSIC_SOURCE_DIR)/Softstone\ Sunset.wav
MUSIC_GAME_SOURCE ?= $(MUSIC_SOURCE_DIR)/Still\ Exploring.wav
MUSIC_RATE ?= 22050
MUSIC_EFFECTS ?= highpass 30 lowpass 10000 gain -n -3
MUSIC_TITLE_PCM := $(AUDIO_BUILD_DIR)/music-title-$(MUSIC_RATE)-mono.wav
MUSIC_GAME_PCM := $(AUDIO_BUILD_DIR)/music-game-$(MUSIC_RATE)-mono.wav
MUSIC_TITLE_AIFC := $(AUDIO_BUILD_DIR)/music-title.vadpcm.aifc
MUSIC_GAME_AIFC := $(AUDIO_BUILD_DIR)/music-game.vadpcm.aifc
MUSIC_TITLE_INFO := $(AUDIO_BUILD_DIR)/music-title.json
MUSIC_GAME_INFO := $(AUDIO_BUILD_DIR)/music-game.json
ASSETS := $(BASE_ASSETS)
SOX ?= sox
VADPCM ?= vadpcm

.DEFAULT_GOAL := default
.PHONY: default clean music music-info FORCE_CODESEGMENT FORCE_ROM

default: $(ROM)

# Run this inside the project Docker image. The encoder accepts mono 16-bit
# PCM only, so SoX supplies the deterministic resample/downmix stage first.
music: $(MUSIC_TITLE_AIFC) $(MUSIC_GAME_AIFC) $(MUSIC_TITLE_INFO) $(MUSIC_GAME_INFO)

music-info: $(MUSIC_TITLE_INFO) $(MUSIC_GAME_INFO)

$(MUSIC_TITLE_PCM): $(MUSIC_TITLE_SOURCE)
	@mkdir -p $(dir $@)
	$(SOX) "$<" -r $(MUSIC_RATE) -c 1 -b 16 -e signed-integer $@ remix - $(MUSIC_EFFECTS)

$(MUSIC_GAME_PCM): $(MUSIC_GAME_SOURCE)
	@mkdir -p $(dir $@)
	$(SOX) "$<" -r $(MUSIC_RATE) -c 1 -b 16 -e signed-integer $@ remix - $(MUSIC_EFFECTS)

$(MUSIC_TITLE_AIFC): $(MUSIC_TITLE_PCM)
	$(VADPCM) encode --predictors 4 $< $@

$(MUSIC_GAME_AIFC): $(MUSIC_GAME_PCM)
	$(VADPCM) encode --predictors 4 $< $@

$(MUSIC_TITLE_INFO): $(MUSIC_TITLE_AIFC) tools/audio_manifest.py
	python3 tools/audio_manifest.py $< $@

$(MUSIC_GAME_INFO): $(MUSIC_GAME_AIFC) tools/audio_manifest.py
	python3 tools/audio_manifest.py $< $@

$(BASE_ASSETS): generate_assets.py tools/import_textures.py $(CUSTOM_TEXTURE_SOURCE)
	python3 generate_assets.py
	@if [ -n "$(CUSTOM_TEXTURE_SOURCE)" ]; then python3 tools/import_textures.py $(CUSTOM_TEXTURE_SOURCE); fi

$(OBJECT_DIR)/%.o: $(SRCDIR)/%.c $(ASSETS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(CODESEGMENT): FORCE_CODESEGMENT $(CODEOBJECTS) Makefile
	$(LD) -o $@ -r $(CODEOBJECTS) $(LDFLAGS)

# Recreate the shared output when switching between cached release/debug
# object trees, even if its timestamp is newer than the selected code segment.
$(ROM): FORCE_ROM $(CODESEGMENT) spec
	$(MAKEROM) spec -I$(NUSYSINCDIR) -r $@ -e $(APP)
	$(MAKEMASK) $@

FORCE_ROM:

FORCE_CODESEGMENT:

clean:
	rm -rf $(OBJDIR)
