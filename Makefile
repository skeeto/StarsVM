# Win16-on-Win32 emulator for Stars! 2.70j
#
# Built with w64devkit.  32-bit is the primary target; the sources stay
# 64-bit-clean so an x64 build is a recompile, not a rewrite - `make CROSS=`
# uses the native toolchain and produces a working x64 emulator.  `make fuzz`
# runs in either mode: its trampoline is emitted for whichever one it was built
# for.  The two builds keep their objects apart, so switching between them does
# not leave `make` thinking it is already up to date and quietly linking the
# wrong architecture.

CROSS   := i686-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
CFLAGS  := -std=c11 -Oz -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wno-unused-parameter -MMD -MP
LDFLAGS := -mwindows -s
LDLIBS  := -luser32 -lgdi32 -lcomdlg32 -lwinmm

SRCDIR  := src
OBJDIR  := build/$(if $(CROSS),32,64)
TARGET  := Stars!VM.exe
RES     := $(OBJDIR)/stars16.res.o

# Both toolchains build the same file name out of different objects, so nothing
# in the dependency graph tells one executable from the other: switching would
# leave `make` with nothing to do and the wrong architecture sitting there.  The
# stamp is the missing edge.  Its recipe always runs but rewrites the file only
# when the architecture actually changed, so it is newer than an executable left
# behind by the other toolchain and older than one it agrees with.
ARCH    := $(if $(CROSS),32,64)
STAMP   := build/arch

# A unity build: src/unity.c includes every other source, so the compiler sees
# the whole program at once.  SRC is still every file, but only to make the one
# object depend on all of them; unity.c itself is excluded so it cannot include
# itself.
SRC := $(filter-out $(SRCDIR)/unity.c,$(wildcard $(SRCDIR)/*.c))
OBJ := $(OBJDIR)/unity.o
DEP := $(OBJ:.o=.d)

.PHONY: all clean imports fuzz FORCE

all: $(TARGET)

$(TARGET): $(OBJ) $(RES) $(STAMP)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LDLIBS)

$(STAMP): FORCE
	@mkdir -p $(@D)
	@test "$$(cat $@ 2>/dev/null)" = "$(ARCH)" || echo $(ARCH) >$@

FORCE:

$(RES): stars16.rc stars16.manifest stars16_icon.rc | $(OBJDIR)
	$(WINDRES) -i $< -o $@

# The icon comes out of the game's own resources.  Never fatal: without the
# game next door the generated .rc is just a comment.
stars16_icon.rc:
	-python tools/mkicon.py "stars.exe" stars16.ico $@ StarsIco
	@test -f $@ || echo "/* no icon */" > $@

$(OBJDIR)/unity.o: $(SRCDIR)/unity.c $(SRC) | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Regenerate the import table from a Wine checkout (interface data only).
imports:
	python tools/genimports.py > $(SRCDIR)/imports.inc

fuzz: $(TARGET)
	./$(TARGET) --fuzz

clean:
	rm -rf build $(TARGET) stars16.ico stars16_icon.rc

-include $(DEP)
