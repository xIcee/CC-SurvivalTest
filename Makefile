# ClassiCube plugin build.
# Default: native Linux .so
# Optional cross-targets from Linux:
#   make windows CC_WIN=x86_64-w64-mingw32-gcc
#   make macos
#
# Notes:
# - Windows cross-builds typically use MinGW.
# - macOS cross-builds default to Zig's Clang frontend targeting x86_64-macos.

BUILD_TARGET ?= native
CLASSICUBE_SRC ?= ClassiCube/src
CLASSICUBE_WIN_IMP ?= ClassiCube/libClassiCube.a
OUTDIR ?= plugins

CC_NATIVE ?= gcc
CC_WIN    ?= x86_64-w64-mingw32-gcc
CC_MAC    ?= zig cc
MAC_TARGET ?= x86_64-macos

CFLAGS_COMMON ?= -std=c99 -Wall -Wextra -O2 -fPIC
LDFLAGS_COMMON ?= -lm

SOURCES = src/plugin.c src/health.c src/deathmsg.c src/motd.c src/policy.c src/respawn.c src/icons.c src/blockbreak.c src/blocktype.c src/texturenoise.c src/blockvalue.c src/tilefood.c src/blockfamily.c src/isoinv.c src/fakeinventory.c src/score.c src/deathscreen.c src/netshim.c src/peer.c src/ccst_grabscreen.c src/persist.c

ifeq ($(BUILD_TARGET),windows)
  TARGET_CC ?= $(CC_WIN)
  TARGET  := $(OUTDIR)/cc_st.dll
  SHARED  := -shared
  CFLAGS  ?= $(CFLAGS_COMMON)
  LDFLAGS ?= $(LDFLAGS_COMMON) -LClassiCube -lClassiCube
else ifeq ($(BUILD_TARGET),macos)
  TARGET_CC ?= $(CC_MAC)
  TARGET  := $(OUTDIR)/cc_st.dylib
  SHARED  := -shared -Wl,-undefined,dynamic_lookup
  CFLAGS  ?= $(CFLAGS_COMMON) -target $(MAC_TARGET)
  LDFLAGS ?= $(LDFLAGS_COMMON)
else
  TARGET_CC ?= $(CC_NATIVE)
  TARGET  := $(OUTDIR)/cc_st.so
  SHARED  := -shared
  CFLAGS  ?= $(CFLAGS_COMMON)
  LDFLAGS ?= $(LDFLAGS_COMMON)
endif

all: $(TARGET)
native:
	$(MAKE) BUILD_TARGET=native
windows:
	$(MAKE) BUILD_TARGET=windows
macos:
	$(MAKE) BUILD_TARGET=macos

$(TARGET): $(SOURCES) | $(OUTDIR)
	$(TARGET_CC) $(SHARED) $(CFLAGS) -o $@ $(SOURCES) -I$(CLASSICUBE_SRC) $(LDFLAGS)

$(OUTDIR)/cc_st.dll: $(CLASSICUBE_WIN_IMP)

$(OUTDIR):
	mkdir -p $(OUTDIR)

clean:
	rm -f "$(OUTDIR)/cc_st.so" "$(OUTDIR)/cc_st.dll" "$(OUTDIR)/cc_st.dylib"

.PHONY: all clean native windows macos
