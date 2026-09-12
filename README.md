# Stars!VM

Runs the 16-bit Windows game **Stars! 2.70j** on modern Windows by
translating it. 64-bit Windows has no NTVDM and cannot execute 16-bit code
at all, which is the version nearly everyone now runs. Stars!VM interprets
the game's machine code itself and its Win16 API calls in terms of Win32,
so it does not care what the host still supports.

![Stars! with a native Windows 11 GUI](docs/screenshot.png)

It is in the same business as [otvdm/winevdm][otvdm], but not the same
kind of thing. otvdm is a general Win16 implementation, built on Wine's
code, that aims to run any Win16 program. This is written from scratch and
aims to run exactly one: every decision is allowed to be settled by what
Stars! 2.70j actually does, which is the only reason 13,000 lines is
enough. Where the two differ, that is usually why — an API this needs to
get exactly right is one the game exercises, and an API it can stub is one
the game never calls.

[otvdm]: https://github.com/otya128/winevdm

What is in the box:

- a 16-bit x86 interpreter (`src/cpu.c`) covering 8086/80186/80286 plus the 386
  additions a 16-bit MS C compiler emits, differentially tested against the host
  CPU by `make fuzz`.  An encoding it does not understand stops the machine
  rather than guessing
- an x87 FPU emulator (`src/fpu.c`), which the game's C runtime requires
- an NE (New Executable) loader: segments, relocations, resources, the entry
  table
- a protected-mode selector model — a reserved arena in which a selector index
  picks a 64 KB slot, so guest far-pointer and huge-pointer arithmetic works
  unchanged
- Win16 → Win32 translation for KERNEL, USER, GDI, COMMDLG, TOOLHELP, WIN87EM,
  MMSYSTEM and WAVEMIX — all 209 imports the game uses, none stubbed

To build and play from source you will need:

- [`stars27jrc3.zip`][dl], just `stars.exe`
  (MD5: `654f494482c7904c4f0f265d6c081582`)
- A [Stars! serial code][starskey]

[dl]: https://wiki.starsautohost.org/wiki/Downloads
[starskey]: https://skeeto.github.io/starskey/

## One file, nothing to install

The emulator will run a module appended to its own executable, so the game and
the shim can be concatenated into a single program:

```bash
cat Stars!VM.exe stars.exe >Stars!-x86.exe
```

That file is all you need. The game is nearly all of it; the emulator adds a
little over 200 KB. No DLLs, no install, no registry. `Stars.ini` is created beside it on first run,
which is also where the game keeps its settings and its registration.

It works because the payload is found by signature: the loader scans its own
image for an MZ header whose `e_lfanew` points at a valid NE. Nothing depends
on arithmetic over where our own binary ends — which is just as well, since
that moves when the linker strips symbols. Alignment is not a concern either:
every offset inside the module stays relative to the module, so the payload can
land anywhere.

## Running it

With no argument, the game is looked for in three places, in order:

1. a path given on the command line
2. a module appended to the emulator's own executable
3. the `Stars!.exe` sitting beside the emulator

Stars! itself is not included and is not redistributable — supply your own copy.

## Building

Built with [w64devkit][w64]. 32-bit is the primary target:

```bash
make
```

64-bit is a recompile rather than a rewrite:

```bash
make CROSS=
```

Both produce a working emulator. The build is a unity build — `src/unity.c`
includes every other source, and the Makefile compiles only that — so the
compiler sees the whole program at once, which is worth having under `-Oz`
where the interpreter loop reaches into the selector and thunk layers on every
instruction. The individual sources are still ordinary `.c` files and each still
compiles standalone, so `gcc -c src/cpu.c` remains available when bisecting a
warning.

[w64]: https://github.com/skeeto/w64devkit

## Sound

The game's battle effects play. Stars! drove them through Microsoft's 1993
`WAVEMIX.DLL`, a software mixer from the days when a sound card could play one
stream; `src/audio.c` reimplements all eleven of its entry points on `waveOut`,
one device per channel, and lets Windows do the mixing. Nothing calls back into
guest code: every device is opened `CALLBACK_NULL` and finished buffers are
recycled by polling, driven from the `WaveMixPump` the game already calls
thirteen times per battle frame.

CD music is deliberately not implemented. The game's soundtrack was Red Book
audio on the retail disc, and it stores a track number and nothing else — there
is no music data anywhere to substitute. `mciSendCommand` reports "no such
device", which makes the game clear its music bit and stop asking. That is what
it did on a machine with no CD in 1995.

## Copy protection

Stars! stamps each submitted turn file with your serial code and an eleven-byte
fingerprint of the machine, and a host penalises one serial appearing under two
different fingerprints. The fingerprint is the volume label, label timestamp and
size of the drives at C: and D:. Stars!VM reported the wrong drive-type numbers
to the guest for a while, which silently reduced that fingerprint to a constant
— the same on every machine — and fixing it invalidates registrations made by
older builds, so the game asks for the serial code once more. `docs/copy-protection.md`
has the disassembly, the byte layout and the parts that are still not faithful.

## Known gaps

- **The decimal adjusts go untested on an x64 build.** `make fuzz` runs in
  either mode, but it works by comparing the interpreter against the host CPU,
  and 64-bit mode deleted DAA, DAS, AAA, AAS, AAM and AAD outright — there is no
  oracle for them there. The run counts those rounds as unrunnable rather than
  as passes, and a 32-bit build covers them. Everything else is tested in both
  modes.
- **Guest-supplied paths are ANSI.** Paths the emulator owns — its own module,
  and `Stars.ini` — are wide throughout, so an installation under a directory
  the ANSI code page cannot spell works. Paths the *game* supplies still go
  through the code page, so a saved game under such a directory will not open.
  Fixing that means choosing an encoding for the guest's bytes.
- WinHelp and printing are stubbed. Modern Windows has no help viewer to
  forward to.

## Debugging

The emulator is a GUI binary, so `--console` is usually wanted alongside these:

| | |
|---|---|
| `--trace-api` | log every Win16 call with its arguments |
| `--trace-cpu N` | log the first N instructions executed |
| `--trace-paint` | log update regions around painting, for repaint loops |
| `--dump`, `--dump-relocs` | print the NE structure |
| `--imports` | print the import thunk table |
| `--peek S:OFF:N` | hex-dump N bytes at segment S, offset OFF |
| `--play-wave N` | play a `WAVE` resource through the sound path and exit |
| `--survey` | keep going past unimplemented APIs instead of stopping |
| `--log FILE` | also write the log to a file |

`--help` lists them all. `tools/` holds PowerShell helpers for driving and
inspecting a running instance — the window tree, update regions, screenshots,
clicking and poking controls — and Python tools for dumping the game's dialog,
menu and icon resources.
