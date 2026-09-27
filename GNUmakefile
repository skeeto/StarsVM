# Win16-on-Win32 emulator for Stars! 2.70j
#
# Built with w64devkit.  32-bit is the primary target; the sources stay
# 64-bit-clean so an x64 build is a recompile, not a rewrite - `make CROSS=`
# uses the native toolchain and produces a working x64 emulator.  The two builds
# keep their objects apart, so switching between them does not leave `make`
# thinking it is already up to date and quietly linking the wrong architecture.
#
# `make fuzz` builds and runs a second, separate program from a subset of the
# same sources.  It stays out of the emulator because its oracle wants a
# writable-executable page; see src/unity_fuzz.c.  It works in either mode, its
# trampoline emitted for whichever one it was built for.

CROSS   := i686-w64-mingw32-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
WINDRES := $(CROSS)windres

# The icon step runs a small program during the build, so that program is built
# for the machine doing the building rather than for the target.  In w64devkit
# `cc` is the native compiler; cross-building elsewhere, it is that host's.
HOSTCC     := cc
HOSTCFLAGS := -std=c99 -O2 -Wall -Wextra
CFLAGS  := -std=c11 -O3 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wno-unused-parameter -MMD -MP -D__USE_MINGW_ANSI_STDIO=0

# FPU picks how the guest's x87 is carried out: hw hands each instruction to
# this machine's own x87.  Empty takes the default for the target.  XCFLAGS is
# for experiments - a rebuild with, say, -falign-functions=64 to see how much
# of a timing difference is only code layout - and keys the build directory
# like everything else here, so it never borrows objects from a build without.
FPU     :=
XCFLAGS :=
ifneq (,$(filter-out hw,$(FPU)))
$(error FPU=$(FPU): the only backend so far is hw)
endif
CFLAGS  += $(if $(FPU),-DSTARSVM_FPU_$(shell echo $(FPU) | tr a-z A-Z)) $(XCFLAGS)
LDFLAGS := -mwindows -s
LDLIBS  := -luser32 -lgdi32 -lcomdlg32 -lwinmm -lshell32 -lshlwapi

# What the compiler targets - i686-w64-mingw32, x86_64-w64-mingw32,
# aarch64-linux-gnu - with the backend and any experimental flags after it.
# Objects live under it, so two builds differing in any of these never share
# one, and the names below follow it rather than the machine doing the building.
TRIPLE  := $(shell $(CC) -dumpmachine)
ARCH    := $(TRIPLE)$(if $(FPU),-fpu-$(FPU))$(if $(XCFLAGS),-x$(shell echo '$(XCFLAGS)' | cksum | cut -d' ' -f1))

SRCDIR  := src
OBJDIR  := build/$(ARCH)
TARGET  := StarsVM.exe
FUZZER  := StarsVM-fuzz.exe
PACKER  := StarsVM-pack.exe
PROF    := StarsVM-prof.exe
HARNESS := StarsVM-harness.exe
ONEFILE := Stars-x86.exe
MKICON  := build/mkicon.exe
RES     := $(OBJDIR)/StarsVM.res.o

# The library (src/stars.h) builds for whatever $(CC) targets, Windows or not,
# so what its files are called follows the compiler rather than this machine.
ifneq (,$(findstring mingw,$(TRIPLE)))
LIBSO   := stars.dll
EXE     := .exe
else
LIBSO   := libstars.so
EXE     :=
PIC     := -fPIC
endif
LIBA    := libstars.a
LIBTEST := $(OBJDIR)/libtest$(EXE)

# Every build makes the same file names out of different objects, so nothing
# in the dependency graph tells one executable from another: switching
# compiler or backend would leave `make` with nothing to do and the wrong
# build sitting there.
#
# A timestamp cannot settle this.  Making the link depend on a stamp rewritten
# when the architecture changes looks right and mostly works, but `make` reads
# the stamp's time when it first considers it and does not reliably read it
# again after the recipe has rewritten it, so whether the switch is noticed
# comes down to its stat cache - which is worse than not working, because it
# works often enough to be believed.
#
# Deleting the executables instead cannot be argued with: this runs while the
# makefile is being read, and `make` stats its targets afterwards and finds them
# missing.  All of them go, not just the one asked for, because after a switch
# all of them are the wrong build.
ARCHFILE := build/arch
$(shell mkdir -p build)
ifneq ($(ARCH),$(shell cat $(ARCHFILE) 2>/dev/null))
$(shell rm -f $(TARGET) $(FUZZER) $(PACKER) $(PROF) $(HARNESS) $(ONEFILE) $(LIBA) $(LIBSO) && echo $(ARCH) >$(ARCHFILE))
endif

# A unity build: src/unity.c includes every other source, so the compiler sees
# the whole program at once, and src/unity_fuzz.c does the same for the eight
# the fuzzer needs.  Both are filtered out of SRC so neither can include itself.
#
# SRC exists only to make the objects depend on every source.  That is
# deliberately over-broad for the fuzzer, which uses eight of them: naming which
# eight here would be a second list to keep in step with unity_fuzz.c, and
# getting it wrong would mean a stale object rather than a rebuild nobody
# noticed.
SRC  := $(filter-out $(SRCDIR)/unity%.c,$(wildcard $(SRCDIR)/*.c))
OBJ  := $(OBJDIR)/unity.o
FOBJ := $(OBJDIR)/unity_fuzz.o
POBJ := $(OBJDIR)/unity_pack.o
ROBJ := $(OBJDIR)/unity_prof.o
HOBJ := $(OBJDIR)/unity_harness.o
LOBJ := $(OBJDIR)/unity_lib.o
SOBJ := $(OBJDIR)/unity_lib_shared.o
DEP  := $(OBJ:.o=.d) $(FOBJ:.o=.d) $(POBJ:.o=.d) $(ROBJ:.o=.d) $(HOBJ:.o=.d) \
        $(LOBJ:.o=.d) $(SOBJ:.o=.d)

.PHONY: all clean imports fuzz onefile prof harness bench lib libtest

all: $(TARGET)

$(TARGET): $(OBJ) $(RES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LDLIBS)

$(RES): StarsVM.rc StarsVM.manifest StarsVM_icon.rc | $(OBJDIR)
	$(WINDRES) -i $< -o $@

# The icon comes out of the game's own resources.  Never fatal: without the
# game next door the generated .rc is just a comment.
StarsVM_icon.rc: $(MKICON)
	-./$(MKICON) stars.exe StarsVM.ico $@ StarsIco
	@test -f $@ || echo "/* no icon */" > $@

# Not under $(OBJDIR): it does not change with the target architecture, so
# switching between the two builds has no reason to rebuild it.
$(MKICON): tools/mkicon.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/mkicon.c

# Each object depends on this makefile as well as on the sources, because
# CFLAGS lives here: without it, changing the optimisation level leaves `make`
# with nothing to do.  Four levels once timed within 0.3% of each other and
# produced byte-identical executables, which is what that looks like.
$(OBJDIR)/unity.o: $(SRCDIR)/unity.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(FOBJ): $(SRCDIR)/unity_fuzz.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(POBJ): $(SRCDIR)/unity_pack.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# unity_prof.c includes unity.c, so it depends on it as well as on the sources.
$(ROBJ): $(SRCDIR)/unity_prof.c $(SRCDIR)/unity.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# The same, for the LLM harness.  Built by `make harness` and never by `all`.
$(HOBJ): $(SRCDIR)/unity_harness.c $(SRCDIR)/unity.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Console programs, so their output needs no --console, and unstripped, because
# a failure in either is exactly when symbols are wanted.  Neither is part of
# the emulator: see the header of the unity file each is built from.
$(FUZZER): $(FOBJ)
	$(CC) $(CFLAGS) -mconsole -o $@ $(FOBJ)

$(PACKER): $(POBJ)
	$(CC) $(CFLAGS) -mconsole -o $@ $(POBJ) -lm

# The emulator again, counters on.  It links what the emulator links, being the
# same program, and it is a console binary so the report needs no --console.
$(PROF): $(ROBJ) $(RES)
	$(CC) $(CFLAGS) -mconsole -o $@ $(ROBJ) $(RES) $(LDLIBS)

# The harness is the emulator with the game's GUI, so it stays a windows binary,
# but it is left unstripped: this is a dev tool and a crash in it is exactly when
# symbols are wanted.
$(HARNESS): $(HOBJ) $(RES)
	$(CC) $(CFLAGS) -mwindows -o $@ $(HOBJ) $(RES) $(LDLIBS)

harness: $(HARNESS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Regenerate the import table from a Wine checkout (interface data only).
imports:
	python tools/genimports.py > $(SRCDIR)/imports.inc

fuzz: $(FUZZER)
	./$(FUZZER)

prof: $(PROF)

# Ten generated turns of the game under bench/, timed and checked against the
# blessed output.  TURNS=50 for the late-game profile.  See tools/bench.ps1.
TURNS := 10
bench: $(TARGET)
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/bench.ps1 -Turns $(TURNS)

# One self-contained executable: the emulator with the game compressed and
# appended.  Needs the game, like the icon step does, and says so rather than
# producing something that cannot run.
onefile: $(TARGET) $(PACKER)
	@test -f stars.exe || { echo "onefile: no stars.exe here to pack"; exit 1; }
	./$(PACKER) stars.exe $(ONEFILE) $(TARGET)

# The library: the game's batch modes with no Win32 and no I/O, for embedding
# and FFI.  See src/stars.h and src/unity_lib.c.  The same code twice, once to
# link statically and once as a DLL or shared object exporting stars.h and
# nothing else.  Any GCC or Clang that targets x86 will do: `make lib CROSS=`
# is the x64 one, and on Linux `make lib CROSS=` gives libstars.so, or with
# CC="cc -m32" a 32-bit one.
lib: $(LIBA) $(LIBSO)

$(LOBJ): $(SRCDIR)/unity_lib.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SOBJ): $(SRCDIR)/unity_lib.c $(SRC) GNUmakefile | $(OBJDIR)
	$(CC) $(CFLAGS) $(PIC) -fvisibility=hidden -DSTARS_SHARED -c -o $@ $<

$(LIBA): $(LOBJ)
	rm -f $@
	$(AR) rcs $@ $(LOBJ)

$(LIBSO): $(SOBJ)
	$(CC) -shared -s -o $@ $(SOBJ)

# The library against the emulator's own output, byte for byte.  Needs the
# game beside the makefile, and uses the benchmark game too when it is there.
# See tests/libtest.c.
libtest: $(LIBTEST)
	./$(LIBTEST) stars.exe $(if $(wildcard bench/golden10),bench)

$(LIBTEST): tests/libtest.c $(SRCDIR)/stars.h $(LIBA)
	$(CC) $(CFLAGS) -o $@ tests/libtest.c $(LIBA) -lpthread

clean:
	rm -rf build $(TARGET) $(FUZZER) $(PACKER) $(PROF) $(ONEFILE) \
	       $(LIBA) $(LIBSO) StarsVM.ico StarsVM_icon.rc

-include $(DEP)
