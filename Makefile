# Win16-on-Win32 emulator for Stars! 2.70j
#
# Built with w64devkit.  32-bit is the primary target; the sources stay
# 64-bit-clean so an x64 build is a recompile, not a rewrite.

CROSS   := i686-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
CFLAGS  := -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wno-unused-parameter -MMD -MP
LDFLAGS := -mwindows
LDLIBS  := -luser32 -lgdi32 -lcomdlg32 -lwinmm

SRCDIR  := src
OBJDIR  := build
TARGET  := Stars!VM.exe
RES     := $(OBJDIR)/stars16.res.o

SRC := $(wildcard $(SRCDIR)/*.c)
OBJ := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRC))
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
	-python tools/mkicon.py "../Stars!.exe" stars16.ico $@ StarsIco
	@test -f $@ || echo "/* no icon */" > $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
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
