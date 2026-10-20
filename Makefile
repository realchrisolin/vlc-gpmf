# GPMF HUD sub-source for VLC 3. Linux, macOS, and Windows.
#
#   make
#   make check
#
# Linux and macOS need the VLC plugin development package (pkg-config vlc-plugin).
# make check does not. It links the reader, the map math, and the zone lookup.
# The plugin looks for timezone21.bin and zoneinfo/ beside itself, then in
# ../third_party/zonedetect/ and ../share/. A development build in build/
# finds third_party/zonedetect/ from this repository.

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
SRCS     = src/plugin.c src/gpmf_read.c src/hud_font.c src/hud_map.c src/hud_tiles.c \
           src/hud_zone.c third_party/zonedetect/zonedetect.c

ifeq ($(OS),Windows_NT)
  OUT      := build/libgpmf_plugin.dll
  VLC_INC  := third_party/vlc/vlc-3.0.23/sdk/include/vlc/plugins
  # The SDK .pc prefix is VideoLAN's CI path, and sdk/lib/*.lib are MSVC
  # import libraries. MinGW links the installed libvlccore.dll instead.
  VLC_DLL  ?= C:/Program Files/VideoLAN/VLC/libvlccore.dll
  CFLAGS   += -I$(VLC_INC) -DMODULE_STRING=\"gpmf\" -D__PLUGIN__ \
              -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE
  # Static libgcc keeps stack probes inside the DLL. VLC will not load libgcc_s or winpthread.
  LDFLAGS  := -shared -static-libgcc
  LIBS     := "$(VLC_DLL)" -lm -lgdi32 -luser32
else
  UNAME_S  := $(shell uname -s)
  # pkg-config --cflags vlc-plugin already passes -D__PLUGIN__. MODULE_STRING
  # is this plugin's name. Hidden visibility keeps the dynamic exports to
  # vlc_entry*, which vlc_plugin.h marks default.
  CFLAGS   += -fPIC -fvisibility=hidden $(shell pkg-config --cflags vlc-plugin) \
              -DMODULE_STRING=\"gpmf\" -D__PLUGIN__ -D_FILE_OFFSET_BITS=64
  ifeq ($(UNAME_S),Darwin)
    OUT     := build/libgpmf_plugin.dylib
    LDFLAGS := -dynamiclib -Wl,-undefined,error
    LIBS    := $(shell pkg-config --libs vlc-plugin) -lm -pthread
  else
    OUT     := build/libgpmf_plugin.so
    LDFLAGS := -shared -Wl,-no-undefined
    LIBS    := $(shell pkg-config --libs vlc-plugin) -lm -ldl -pthread
  endif
endif

PREFIX    ?= /usr/local
plugindir := $(PREFIX)/lib/vlc/plugins/misc

CHECK_SRCS = tests/check.c src/gpmf_read.c src/hud_map.c src/hud_zone.c \
             third_party/zonedetect/zonedetect.c
ifeq ($(OS),Windows_NT)
  CHECK_BIN  := build/gpmf-check.exe
  CHECK_LIBS := -static-libgcc -lm
  CHECK_NEED := $(OUT)
else
  CHECK_BIN  := build/gpmf-check
  CHECK_NEED :=
  ifeq ($(UNAME_S),Darwin)
    CHECK_LIBS := -lm -pthread
  else
    CHECK_LIBS := -lm -ldl -pthread
  endif
endif

.PHONY: all clean check install uninstall

all: $(OUT)

check: $(CHECK_NEED) $(CHECK_BIN)
	$(CHECK_BIN)

$(OUT): $(SRCS) | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)

$(CHECK_BIN): $(CHECK_SRCS) | build
	$(CC) -std=c11 -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -Isrc -o $@ $(CHECK_SRCS) $(CHECK_LIBS)

ifeq ($(OS),Windows_NT)
build:
	if not exist build mkdir build
else
build:
	mkdir -p build
endif

clean:
	$(RM) $(OUT) $(CHECK_BIN) build/check-seam.mp4 build/check-gps9.mp4 build/check-file.mp4

ifeq ($(OS),Windows_NT)
install:
	@echo Copy $(OUT) into VLC's plugins\misc, or set VLC_PLUGIN_PATH. See README.md.
uninstall:
	@echo Remove libgpmf_plugin.dll from VLC's plugins\misc.
else
install: all
	mkdir -p $(DESTDIR)$(plugindir)
	install -m 0755 $(OUT) $(DESTDIR)$(plugindir)/
uninstall:
	rm -f $(DESTDIR)$(plugindir)/libgpmf_plugin.so $(DESTDIR)$(plugindir)/libgpmf_plugin.dylib
endif
