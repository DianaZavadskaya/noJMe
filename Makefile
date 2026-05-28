# J2ME Libretro Core - Cross-compilation Makefile
# Default: Windows (MinGW)
# Supports: Windows (default), Linux (native), M17 ARM 32-bit (cross)
#
# Build targets:
#   make                        - Build libretro core (Windows, default)
#   make platform=windows       - Same as default, explicitly Windows
#   make platform=linux         - Build libretro core (native Linux, .so)
#   make platform=m17           - Build for M17 ARM 32-bit (.so, release, stripped)
#   make app                    - Build standalone application (native)
#   make clean                  - Clean all build artifacts
#
# When running inside MSYS2/MinGW shell on Windows, the native gcc is used
# automatically (no cross-compiler needed). CROSS_COMPILE is only used when
# building FROM Linux/other hosts.
#
# M17 cross-compilation:
#   make platform=m17 CROSS_COMPILE=/opt/m17-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-
#
# M17 release build: -Ofast, no debug symbols, stripped .so
#

# ==============================================
# Default settings
# ==============================================
TARGET_NAME := nojme
SRCDIR := src
INCDIR := include
OBJDIR := obj
BINDIR := bin

# Source files
JVM_SRCS := \
	$(SRCDIR)/jvm/classfile.c \
        $(SRCDIR)/jvm/debug_var.c \
	$(SRCDIR)/jvm/execute.c \
	$(SRCDIR)/jvm/heap.c \
	$(SRCDIR)/jvm/jvm.c \
	$(SRCDIR)/jvm/method_cache.c \
	$(SRCDIR)/jvm/native.c \
	$(SRCDIR)/jvm/nokia_m3d.c \
	$(SRCDIR)/jvm/opcodes.c \
	$(SRCDIR)/jvm/stubs.c \
	$(SRCDIR)/jvm/threads.c

MIDP_SRCS := \
	$(SRCDIR)/midp/display.c \
	$(SRCDIR)/midp/form.c \
	$(SRCDIR)/midp/graphics.c \
	$(SRCDIR)/midp/media.c \
	$(SRCDIR)/midp/mobile3d.c \
	$(SRCDIR)/midp/rms.c

UTIL_SRCS := \
	$(SRCDIR)/utils/miniz.c \
	$(SRCDIR)/utils/stb_image_impl.c \
	$(SRCDIR)/utils/utils.c \
	$(SRCDIR)/jvm/drm_bypass.c

MIDI_SRCS := \
	$(SRCDIR)/midi/midi.c

LIBRETRO_SRCS := \
	$(SRCDIR)/libretro/libretro.c \
	$(SRCDIR)/libretro/sdl_backend_stubs.c

# SDL sources (for standalone app only)
SDL_SRCS := $(SRCDIR)/sdl/sdl_graphics.c

ALL_LIBRETRO_SRCS := $(JVM_SRCS) $(MIDP_SRCS) $(UTIL_SRCS) $(MIDI_SRCS) $(LIBRETRO_SRCS)
ALL_APP_SRCS := $(JVM_SRCS) $(MIDP_SRCS) $(UTIL_SRCS) $(MIDI_SRCS) $(SDL_SRCS) $(SRCDIR)/main.c

# ==============================================
# Detect build environment
# ==============================================
# MSYSTEM is set inside MSYS2/MinGW shells (MINGW32, MINGW64, UCRT64, CLANG64, etc.)
# MINGW_PREFIX is set inside MinGW environments
# If either is set, we are ALREADY on Windows and gcc produces Windows binaries natively.
IS_MINGW_NATIVE := $(if $(MSYSTEM),1,$(if $(MINGW_PREFIX),1,0))

# ==============================================
# ARM-specific optimizations (used for all ARM targets)
# ==============================================
ARM_OPT_CFLAGS := -ftree-vectorize -fipa-cp-clone -finline-functions-called-once

# ==============================================
# Platform selection
# ==============================================

ifeq ($(platform), m17)
        # ==============================================
        # M17 ARM 32-bit cross-compilation (release, stripped)
        # ==============================================
	TARGET := $(BINDIR)/$(TARGET_NAME)_libretro.so

	CROSS_COMPILE ?= /opt/m17-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-
	CC = $(CROSS_COMPILE)gcc
	CXX = $(CROSS_COMPILE)g++
	AR = $(CROSS_COMPILE)ar
	STRIP = $(CROSS_COMPILE)strip

        # ARM 32-bit optimization flags — release, no debug
	CFLAGS := -Wall -Wextra -std=c11 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include -I$(SRCDIR)/jvm -I$(SRCDIR)/midp \
	-I$(SRCDIR)/utils -I$(SRCDIR)/libretro \
	-DLIBRETRO -DARM -D_GNU_SOURCE -DJ2ME_DEBUG=0 -DOPT_NO_BOUNDS_CHECK \
	-Ofast -fPIC \
	-fdata-sections -ffunction-sections \
	-fno-stack-protector -fomit-frame-pointer \
	-fmerge-all-constants -fno-math-errno \
	-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-flto $(ARM_OPT_CFLAGS)

	LDFLAGS := -shared -Wl,--no-undefined -Wl,--gc-sections \
	-Wl,--version-script=$(SRCDIR)/libretro/link.T \
	-flto -fuse-linker-plugin \
	-lm -pthread

	HAVE_NEON = 1
	ARCH = arm
	PLATFORM_NAME = "M17 ARM 32-bit"

else ifeq ($(platform), linux)
        # ==============================================
        # Native Linux build
        # ==============================================
	TARGET := $(BINDIR)/$(TARGET_NAME)_libretro.so

	CC ?= gcc
	CXX ?= g++
	AR ?= ar
	STRIP ?= strip

        # Detect architecture
	UNAME_M := $(shell uname -m)
	ifeq ($(UNAME_M),x86_64)
	ARCH = x86_64
	ARCH_CFLAGS = -march=native
	else ifeq ($(UNAME_M),aarch64)
	ARCH = arm64
	ARCH_CFLAGS = -march=armv8-a
	else ifeq ($(UNAME_M),armv7l)
	ARCH = arm
	ARCH_CFLAGS = -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
	else
	ARCH = $(UNAME_M)
	ARCH_CFLAGS =
	endif

	CFLAGS := -Wall -Wextra -std=c11 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
	-DLIBRETRO -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
	-O2 -fPIC $(ARCH_CFLAGS)

	LDFLAGS := -shared -Wl,--no-undefined \
	-Wl,--version-script=$(SRCDIR)/libretro/link.T \
	-lm -pthread

	PLATFORM_NAME = "Linux $(ARCH)"

        # ARM-specific optimizations
	ifeq ($(ARCH),arm)
	CFLAGS += -O3 -flto $(ARM_OPT_CFLAGS)
	LDFLAGS += -flto -fuse-linker-plugin
	endif

else
        # ==============================================
        # Windows (MinGW) — DEFAULT
        # ==============================================
	TARGET := $(BINDIR)/$(TARGET_NAME)_libretro.dll

ifeq ($(IS_MINGW_NATIVE),1)
        # ---- Running inside MSYS2/MinGW on Windows ----
	CC ?= gcc
	CXX ?= g++
	AR ?= ar
	STRIP ?= strip

        # Detect 32/64 bit from MSYSTEM or uname
	IS_64BIT := $(if $(findstring 64,$(MSYSTEM)),1,0)
	ifeq ($(IS_64BIT),1)
	ARCH = x86_64
	else
	ARCH = x86
	endif
	PLATFORM_NAME = "Windows $(ARCH) (native MinGW)"
else
        # ---- Cross-compiling from Linux to Windows ----
	CROSS_COMPILE ?= x86_64-w64-mingw32-
	CC = $(CROSS_COMPILE)gcc
	CXX = $(CROSS_COMPILE)g++
	AR = $(CROSS_COMPILE)ar
	STRIP = $(CROSS_COMPILE)strip

	ARCH = x86_64
	PLATFORM_NAME = "Windows $(ARCH) (cross-compile)"
endif

	CFLAGS := -Wall -Wextra -std=c11 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
	-DLIBRETRO -DWIN32 -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
	-g -O2 -fPIC \
	-D__USE_MINGW_ANSI_STDIO=1

	LDFLAGS := -shared -static-libgcc -static-libstdc++ \
	-Wl,--out-implib,$(BINDIR)/lib$(TARGET_NAME)_libretro.a \
	-lm
endif

# ==============================================
# Object files (all go to obj/<build-type>/)
# ==============================================
ALL_LIBRETRO_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/libretro/%.o,$(ALL_LIBRETRO_SRCS))

# ==============================================
# Build targets
# ==============================================

.PHONY: all clean app dirs check info libretro headless baseline test

# Default: build libretro core (Windows by default)
all: libretro

# Build libretro core
libretro: check_compiler dirs $(TARGET)
	@echo ""
	@echo "============================================"
	@echo "Libretro core built successfully!"
	@echo "============================================"
	@echo "Output:    $(TARGET)"
	@echo "Platform:  $(PLATFORM_NAME)"
	@echo "Arch:      $(ARCH)"
	@echo "Compiler:  $(CC)"
	@echo "Objects:   $(OBJDIR)/"
	@echo ""
	@ls -lh $(TARGET) 2>/dev/null || true
	@file $(TARGET) 2>/dev/null || true

# Check compiler availability
check_compiler:
	@echo "Checking compiler: $(CC)"
	@$(CC) --version >/dev/null 2>&1 || { \
	echo "ERROR: $(CC) not found!"; \
	echo "Please install the toolchain or set CC variable."; \
	exit 1; \
        }
	@echo "Compiler found: $(shell $(CC) --version | head -1)"

# Create directories
dirs:
	@echo "Creating directories..."
	@mkdir -p $(OBJDIR)/libretro/jvm
	@mkdir -p $(OBJDIR)/libretro/midp
	@mkdir -p $(OBJDIR)/libretro/utils
	@mkdir -p $(OBJDIR)/libretro/midi
	@mkdir -p $(OBJDIR)/libretro/libretro
	@mkdir -p $(BINDIR)
	@echo "Directories created"

# Compile rules — objects go to obj/libretro/<subdir>/
$(OBJDIR)/libretro/jvm/%.o: $(SRCDIR)/jvm/%.c
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libretro/midp/%.o: $(SRCDIR)/midp/%.c
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libretro/utils/%.o: $(SRCDIR)/utils/%.c
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libretro/midi/%.o: $(SRCDIR)/midi/%.c
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libretro/libretro/%.o: $(SRCDIR)/libretro/%.c
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Link
$(TARGET): $(ALL_LIBRETRO_OBJS)
	@echo "  LD  $(TARGET)"
	$(CC) $^ -o $@ $(LDFLAGS)
ifeq ($(platform),m17)
	@echo "  STRIP $(TARGET) (release)"
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true
else ifneq ($(STRIP),)
	@echo "  STRIP $(TARGET) (disabled for debug)"
	@# $(STRIP) --strip-unneeded $@ 2>/dev/null || true
endif
	@echo "Linked successfully"

# ==============================================
# Standalone application (native only)
# ==============================================
APP_TARGET := $(BINDIR)/j2me-emulator
APP_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/app/%.o,$(ALL_APP_SRCS))

# SDL2 detection
SDL2_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || echo "")

APP_CFLAGS := -Wall -Wextra -std=c11 -O2 -I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include -D_GNU_SOURCE $(SDL2_CFLAGS)
APP_LDFLAGS := -lm -pthread $(SDL2_LIBS)

app: app-dirs $(APP_TARGET)
	@echo ""
	@echo "============================================"
	@echo "Application built successfully!"
	@echo "============================================"
	@echo "Output: $(APP_TARGET)"
	@ls -lh $(APP_TARGET)

app-dirs:
	@mkdir -p $(OBJDIR)/app/jvm $(OBJDIR)/app/midp $(OBJDIR)/app/sdl \
	$(OBJDIR)/app/utils $(OBJDIR)/app/midi $(BINDIR)

$(APP_TARGET): $(APP_OBJS)
	$(CC) $^ -o $@ $(APP_LDFLAGS)

$(OBJDIR)/app/%.o: $(SRCDIR)/%.c
	$(CC) $(APP_CFLAGS) -c $< -o $@

# ==============================================
# Headless application (no SDL2 required)
# ==============================================
HEADLESS_TARGET := $(BINDIR)/j2me-headless
HEADLESS_SRCS := $(JVM_SRCS) $(MIDP_SRCS) $(UTIL_SRCS) $(MIDI_SRCS) \
	$(SRCDIR)/sdl/sdl_headless.c $(SRCDIR)/main.c
HEADLESS_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/headless/%.o,$(HEADLESS_SRCS))
HEADLESS_CC ?= gcc

HEADLESS_CFLAGS := -Wall -Wextra -Wno-unused-function -std=c11 -O2 -I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include -D_GNU_SOURCE -DJ2ME_DEBUG=1 -DJ2ME_HEADLESS
HEADLESS_LDFLAGS := -lm -pthread

headless: headless-dirs $(HEADLESS_TARGET)
	@echo ""
	@echo "============================================"
	@echo "Headless application built successfully!"
	@echo "============================================"
	@echo "Output: $(HEADLESS_TARGET)"
	@ls -lh $(HEADLESS_TARGET)

headless-dirs:
	@mkdir -p $(OBJDIR)/headless/jvm $(OBJDIR)/headless/midp $(OBJDIR)/headless/sdl \
	$(OBJDIR)/headless/utils $(OBJDIR)/headless/midi $(BINDIR)

$(HEADLESS_TARGET): $(HEADLESS_OBJS)
	$(HEADLESS_CC) $^ -o $@ $(HEADLESS_LDFLAGS)

$(OBJDIR)/headless/%.o: $(SRCDIR)/%.c
	$(HEADLESS_CC) $(HEADLESS_CFLAGS) -c $< -o $@

# ==============================================
# Utility targets
# ==============================================

# Clean all object and output files
clean:
	@echo "Cleaning $(OBJDIR)/ $(BINDIR)/"
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "Cleaned"

# Check source files
check:
	@echo "=== Checking source files ==="
	@for src in $(ALL_LIBRETRO_SRCS); do \
	if [ -f "$$src" ]; then \
	echo "  OK  $$src"; \
	else \
	echo "  MISSING  $$src"; \
	exit 1; \
	fi \
	done
	@echo "=== All source files present ==="
	@echo "Total: $(words $(ALL_LIBRETRO_SRCS)) files"

# Show build info
info:
	@echo "=== Build Configuration ==="
	@echo "Platform:       $(PLATFORM_NAME)"
	@echo "Target:         $(TARGET)"
	@echo "Compiler:       $(CC)"
	@echo "Arch:           $(ARCH)"
	@echo "Obj directory:  $(OBJDIR)/"
	@echo "MinGW native:   $(IS_MINGW_NATIVE)"
	@echo "MSYSTEM:        $(MSYSTEM)"
	@echo ""
	@echo "=== Compiler Flags ==="
	@echo "CFLAGS:  $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo ""
	@echo "=== Source files ==="
	@echo "JVM:      $(words $(JVM_SRCS)) files"
	@echo "MIDP:     $(words $(MIDP_SRCS)) files"
	@echo "Utils:    $(words $(UTIL_SRCS)) files"
	@echo "MIDI:     $(words $(MIDI_SRCS)) files"
	@echo "Libretro: $(words $(LIBRETRO_SRCS)) files"
	@echo "Total:    $(words $(ALL_LIBRETRO_SRCS)) files"

# ==============================================
# Regression oracle targets
# ==============================================

baseline: headless
	@mkdir -p tests/baselines
	@sh tests/capture-baseline.sh

test: headless
	@sh tests/check-regression.sh
