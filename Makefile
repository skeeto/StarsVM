# Win16-on-Win32 emulator for Stars! 2.70j
#
# Built with w64devkit.  32-bit is the primary target; the sources stay
# 64-bit-clean so an x64 build is a recompile, not a rewrite - `make CROSS=`
# uses the native toolchain and produces a working x64 emulator.  The one thing
# that does not survive is `make fuzz`, whose trampoline is 32-bit machine code;
# it says so and stops rather than pretending.

CROSS   := i686-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
CFLAGS  := -std=c11 -Oz -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wno-unused-parameter -MMD -MP
LDFLAGS := -mwindows -s
LDLIBS  := -luser32 -lgdi32 -lcomdlg32 -lwinmm

SRCDIR  := src
OBJDIR  := build
TARGET  := Stars!VM.exe
RES     := $(OBJDIR)/stars16.res.o

# A unity build: src/unity.c includes every other source, so the compiler sees
# the whole program at once.  SRC is still every file, but only to make the one
# object depend on all of them; unity.c itself is excluded so it cannot include
# itself.
SRC := $(filter-out $(SRCDIR)/unity.c,$(wildcard $(SRCDIR)/*.c))
OBJ := $(OBJDIR)/unity.o
DEP := $(OBJ:.o=.d)

.PHONY: all clean imports fuzz

all: $(TARGET)

$(TARGET): $(OBJ) $(RES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LDLIBS)

$(RES): stars16.rc stars16.manifest stars16_icon.rc | $(OBJDIR)
	$(WINDRES) -i $< -o $@

# The icon comes out of the game's own resources.  Never fatal: without the
# game next door the generated .rc is just a comment.
stars16_icon.rc:
	-python tools/mkicon.py "Stars!.exe" stars16.ico $@ StarsIco
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
	rm -rf $(OBJDIR) $(TARGET) stars16.ico stars16_icon.rc

-include $(DEP)
