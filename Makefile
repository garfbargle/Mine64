# Modern N64 SDK-compatible build for Mine64 v0.2.
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

LCDEFS := -DNU_DEBUG -DF3DEX_GBI_2
LCINCS := -I. -I$(NUSYSINCDIR) -I$(ROOT)/usr/include/PR -I$(INCDIR) -I$(INCDIR)/ff -I$(ASSDIR) -I$(ROOT)/usr/include
LCOPTS := -mips3 -mgp32 -mfp32 -funsigned-char -fcommon -D_LANGUAGE_C -D_ULTRA64 -D__EXTENSIONS__
LDFLAGS := $(MKDEPOPT) -L$(LIB) -L$(NUSYSLIBDIR) -lnusys_d -lultra_d -lcart

OPTIMIZER := -g

APP := $(OBJDIR)/$(TARGET).out
ROM := $(OBJDIR)/$(TARGET).n64
CODEFILES := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/ff/*.c)
CODEOBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CODEFILES)) $(NUSYSLIBDIR)/nusys_d.o
CODESEGMENT := $(OBJDIR)/codesegment.o
ASSETS := $(ASSDIR)/texture_data.h $(ASSDIR)/font.h

.DEFAULT_GOAL := default
.PHONY: default clean

default: $(ROM)

$(ASSETS): generate_assets.py
	python3 generate_assets.py

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(ASSETS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(CODESEGMENT): $(CODEOBJECTS) Makefile
	$(LD) -o $@ -r $(CODEOBJECTS) $(LDFLAGS)

$(ROM): $(CODESEGMENT) spec
	$(MAKEROM) spec -I$(NUSYSINCDIR) -r $@ -e $(APP)
	$(MAKEMASK) $@

clean:
	rm -rf $(OBJDIR)
