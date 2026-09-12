# Copy protection: the serial code and the machine fingerprint

Stars! stamps every turn file you submit with two numbers: your serial code and
an eleven-byte "computer hardware code" derived from the machine. A host
running the turn generator penalises a serial that turns up under two different
hardware codes in one turn — that is the whole scheme. This documents what the
hardware code actually is, because Stars!VM got it wrong for a while and the
way it got it wrong was invisible.

Addresses below are `segment:offset` into `stars.exe` (2.70j, 3,153,152
bytes). To reach a file offset, add the segment's base; `Stars!VM.exe --dump`
prints them.

## Where it lives

| what | where |
|---|---|
| producer | `seg9:0x4620`–`0x4835`, no arguments, returns 11 in AX |
| output | DGROUP (segment 38, autodata) offset `0x5488`, 11 bytes |
| only caller | `seg1:0x07C1`, on the bare startup path, once per process |
| the check | `seg5:0x142E`–`0x144B` |
| copies after registering | `seg5:0x14C2`, `seg5:0x14F3`, `seg7:0x0CCA` |
| turn-file stamp | `seg10:0x81D2`–`0x8205` |

The check is an eleven-byte `memcmp` between `DGROUP:0x51BE` (decoded from
`Stars.ini`) and `DGROUP:0x5488` (computed just now), compiled inline as
`shr cx,1` / `repe cmpsw` / `repe cmpsb`. It is guarded by
`cmp dword ds:[086C],0` — the 32-bit serial. A zero serial or a mismatch raises
dialog template 86, `#32770 "Stars! Serial Number"`, whose text is *"Your
machine configuration appears to have changed. Please re-enter the serial
number."* This runs at startup, before any game is opened.

## What it reads

`seg9:0x4620` is **statically closed**, which is what makes the input list below
exhaustive rather than merely observed. Across its whole extent there are five
call instructions, no indirect calls, no `INT`/`IN`/`OUT`/`CPUID`, and no reads
of DGROUP globals at all — every memory operand is a local, the DTA it just
filled, or its write-only output. Nothing stashed away at startup can reach it.

Two loops, each `for (i = 0; i < 2; i++)` over drives **C: and D: only**. Not a
drive walk; that is why a trace shows exactly `GetDriveType(2)`,
`GetDriveType(3)` twice.

Both loops gate on `GetDriveType() == 3`, which on Win16 is `DRIVE_FIXED` — an
ordinary hard disk. There are exactly two `KERNEL.136` call sites in the whole
binary, `seg9:0x4667` and `seg9:0x47A4`, and both are followed by `3D 03 00`.

1. **Volume label.** `_dos_findfirst(path, _A_VOLID)` on `"C:\*.*"` (string
   table entry 1225, first byte overwritten per drive). The label text is
   folded up to 8 characters — C: as `acc = (acc << 4) | (ch & 0x0F)`, D: as
   `acc = (acc << 3) | (ch & 0x07)` — and the label's FAT timestamp is
   bit-packed, with a different formula per drive.
2. **Capacity.** `_dos_getdiskfree`, then
   `bytes_per_sector * total_clusters / 512 * sectors_per_cluster / 195312`,
   clamped to 15 — total size in roughly 100 MB units. It reads the *total*
   cluster count, not the free one, so **free disk space is not an input**.

Byte layout:

| bytes | source |
|---|---|
| 0–3 | C: label fold (32-bit, little-endian) |
| 4–5 | C: label timestamp |
| 6–8 | D: label fold (3 bytes; the 4th store is skipped at `seg9:0x4769`) |
| 9 | D: label timestamp, low byte only (high skipped at `seg9:0x477F`) |
| 10 | `(C: capacity nibble << 4) | D: capacity nibble` |

When a gate fails or the label search errors, the code takes a fallback of two
instruction immediates, `mov word [bp-8],0C57Ah` at `seg9:0x4734` and
`mov dword [bp-4],59A6DCA5h` at `seg9:0x4739`. Both byte sequences occur
exactly once in the binary.

## Persistence and the turn file

`Stars.ini [Windows] GlobalSettings` is a 28-character blob holding a 21-byte
struct: 6 bits per character, LSB-first, over the alphabet `A-Z a-z 0-9 - *`,
then de-scrambled by the 21-entry permutation at `seg5 cs:0x02B5`. Decoder
`seg5:0x1E98`, encoder `seg5:0x1D12`.

```
[0..3]    32-bit serial value V   (DGROUP:0x086C)
[4..14]   the 11-byte hardware code (DGROUP:0x5488)
[15..20]  check values; xor(bytes[0..14]) sits in the high nibble of [20]
```

The name is a red herring: it holds no settings. `V` is the same polynomial
`docs/starskey.c` computes from the eight-character serial. The length test
`cmp ax,0x1C` at `seg1:0x1118` is why deleting or truncating the line
re-prompts — a short read leaves `DGROUP:0x086C` zero. The game rewrites this
key on every clean exit, which makes it a convenient read-out of the live
hardware code.

In a submitted turn file the stamp is block **type 9**, 17 bytes:
`{ word DGROUP:0x964; dword DGROUP:0x086C; byte[11] DGROUP:0x5488 }`. It appears
in `.x` files and in no other family, which matches the folklore exactly. At
turn 0 with no orders, a `.x1` is a type 8 header, this block, and a footer —
39 bytes, almost all of it copy protection.

## The bug Stars!VM had

`k_GetDriveType` mapped the Win32 drive types onto `0` absent, `1` removable,
`2` fixed, `3` remote. That numbering is wrong. Win16 used the same values
Win32 later kept — removable 2, fixed 3, remote 4 — with only two differences,
both of which Wine's `GetDriveType16` still reproduces: MSCDEX reached a CD-ROM
through the network redirector, so one reports *remote*, and a root that is not
there reports *unknown*.

Off by one, every hard disk reported as removable, so `cmp ax,3` never matched,
so both loops took the fallback, so the hardware code on **every Stars!VM
installation on every machine** was the same eleven bytes:

```
A5 DC A6 59 7A C5 A5 DC A6 7A 00
```

which is just the two fallback immediates written out twice. The scheme was not
weakened; it was switched off. Worse, the mapping was *inverted* rather than
merely broken: `DRIVE_REMOTE` landed on 3, so the only machine that got a real
fingerprint was one with a network drive at C: or D: — the exact opposite of
what the game intends.

Three fixes were needed together:

- `api_kernel.c` `k_GetDriveType` — pass the Win32 value through, remapping
  only CD-ROM to remote and no-root-dir to unknown, exactly as Win16 did.
- `api_dos.c` int 21h `AH=4Eh` — a search for exactly the volume-label
  attribute has to be answered from `GetVolumeInformation`. `FindFirstFile`
  never reports a label, so before this the game hashed whatever ordinary
  directory entry came back first.
- `api_dos.c` int 21h `AH=36h` — the drive arrives in **DL**, not AL. Reading
  AL asked Win32 about a drive named by whatever was left in the register, so
  the handler had never once succeeded. Nothing noticed, because its only
  caller was the dead capacity branch.

Worked example, after the fix, on a machine whose C: is labelled `OS` and D:
`DATA`, both large fixed disks: `OS` folds 4 bits per character to `0x000000F3`
and `DATA` folds 3 bits per character to `0x00000861`, giving

```
F3 00 00 00 00 00 61 08 00 00 FF
```

which is what the game computes. The two timestamp fields are zero because
Win32 cannot reach a volume label's FAT timestamp; see the caveat below.

**Fixing this invalidates every registration made by an older Stars!VM.** The
stored constant no longer matches, so the game asks for the serial code once
more. That is correct behaviour — the machine really did change — but it is a
one-time annoyance for anyone upgrading.

## Consequences

- Before the fix, two people sharing one serial on Stars!VM were
  indistinguishable and never flagged; the protection was structurally
  defeated, not merely weakened. An honest player alternating between Stars!VM
  and a real install was flagged, because their two environments disagreed.
  Both are now correct.
- The constant was also a signature: a host operator who knew those eleven
  bytes could pick Stars!VM turn files out at a glance.
- The fingerprint still tracks what it always did — rename a volume label, or
  add or remove a drive at C: or D:, and the registration is invalidated. That
  is the game's design, not a defect.

## What is not faithful, and what is unverified

- **Label timestamps are zero.** Win32 has no route to a volume label's
  directory-entry timestamp; on NTFS there is not even an entry to read. Bytes
  4, 5 and 9 are therefore constant. Two machines whose C: and D: labels and
  capacities agree will collide where a real Win16 box would not. Synthesising
  something from the volume serial number would add entropy but would not be
  what the game would have seen, so it is left alone.
- Only the bare volume-label attribute is redirected in `AH=4Eh`. Real DOS also
  returns the label when other attribute bits accompany it; those searches keep
  the ordinary file path here, because nothing in the game needs otherwise and
  injecting a label into a directory enumeration is the riskier error.
- **The host side is untested.** That the turn generator compares block-9 codes
  across submissions and penalises a mismatch is taken from the third-party
  page this investigation started from, reproduced at
  `copy-protection-features.md`. Nothing here tested it; producing the test
  case needs two `.x` files with different hardware codes for one serial.
- No ground-truth sample of a hardware code from a real Windows 3.1 machine was
  ever obtained. Everything about what real Win16 computes is read out of the
  disassembly.
- That page's claim about the hard disk label is correct, and so is its claim
  that the codes live in the `.x` files. Its claim that the code changes with
  "adding/removing a piece of hardware" is true only of storage at C: or D:.
  Its implication that free disk space matters is wrong.
