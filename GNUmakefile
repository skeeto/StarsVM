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
WINDRES := $(CROSS)windres
CFLAGS  := -std=c11 -O3 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wno-unused-parameter -MMD -MP -D__USE_MINGW_ANSI_STDIO=0
LDFLAGS := -mwindows -s
LDLIBS  := -luser32 -lgdi32 -lcomdlg32 -lwinmm

SRCDIR  := src
OBJDIR  := build/$(if $(CROSS),32,64)
TARGET  := StarsVM.exe
FUZZER  := StarsVM-fuzz.exe
PACKER  := StarsVM-pack.exe
PROF    := StarsVM-prof.exe
ONEFILE := Stars-x86.exe
RES     := $(OBJDIR)/StarsVM.res.o

# Both toolchains build the same file names out of different objects, so nothing
# in the dependency graph tells one executable from the other: switching would
# leave `make` with nothing to do and the wrong architecture sitting there.
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
# missing.  Both go, not just the one asked for, because after a switch both are
# the wrong architecture.
ARCH     := $(if $(CROSS),32,64)
ARCHFILE := build/arch
$(shell mkdir -p build)
ifneq ($(ARCH),$(shell cat $(ARCHFILE) 2>/dev/null))
$(shell rm -f $(TARGET) $(FUZZER) $(PACKER) $(PROF) $(ONEFILE) && echo $(ARCH) >$(ARCHFILE))
endif

# A unity build: src/unity.c includes every other source, so the compiler sees
# the whole program at once, and src/unity_fuzz.c does the same for the seven
# the fuzzer needs.  Both are filtered out of SRC so neither can include itself.
#
# SRC exists only to make the objects depend on every source.  That is
# deliberately over-broad for the fuzzer, which uses seven of them: naming which
# seven here would be a second list to keep in step with unity_fuzz.c, and
# getting it wrong would mean a stale object rather than a rebuild nobody
# noticed.
SRC  := $(filter-out $(SRCDIR)/unity%.c,$(wildcard $(SRCDIR)/*.c))
OBJ  := $(OBJDIR)/unity.o
FOBJ := $(OBJDIR)/unity_fuzz.o
POBJ := $(OBJDIR)/unity_pack.o
ROBJ := $(OBJDIR)/unity_prof.o
DEP  := $(OBJ:.o=.d) $(FOBJ:.o=.d) $(POBJ:.o=.d) $(ROBJ:.o=.d)

.PHONY: all clean imports fuzz onefile prof bench

all: $(TARGET)

$(TARGET): $(OBJ) $(RES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LDLIBS)

$(RES): StarsVM.rc StarsVM.manifest StarsVM_icon.rc | $(OBJDIR)
	$(WINDRES) -i $< -o $@

# The icon comes out of the game's own resources.  Never fatal: without the
# game next door the generated .rc is just a comment.
StarsVM_icon.rc:
	-python tools/mkicon.py stars.exe StarsVM.ico $@ StarsIco
	@test -f $@ || echo "/* no icon */" > $@

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

clean:
	rm -rf build $(TARGET) $(FUZZER) $(PACKER) $(PROF) $(ONEFILE) \
	       StarsVM.ico StarsVM_icon.rc

-include $(DEP)
