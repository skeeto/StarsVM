# Native routines: the game's hottest code, rewritten

Turn generation is the one slow thing in Stars!VM, and it is slow in a very
particular way. `make prof` on ten generated turns of a Huge, packed,
16-player game shows a quarter of all instructions executed inside five basic
blocks, half inside twenty, and three quarters inside 139 — out of 56,000
distinct instruction addresses that run at all. The hot code is not the
interpreter's problem in general; it is a handful of small, closed pieces of
game logic run tens of millions of times: the nearest-object scan, the
habitability formula, the random generator.

So those pieces are rewritten in C and patched over the guest code. The first
byte of each site becomes `0xD6`, an opcode no 16-bit compiler emits (SALC),
and the interpreter hands that opcode to the routine instead of decoding it.
`src/native.c` holds the routines; this file is their documentation: the
disassembly each stands in for, where it stops, and what it must leave behind.

Addresses are `segN:offset` into `stars.exe` 2.70j, as in
`docs/copy-protection.md`. Each site is patched only if the eight bytes there
are the ones the routine was written against.

## The contract

A routine keeps the interpreter's contract, not the game's. Starting at its
site it may perform any number of complete guest instructions, and it must
stop at an instruction boundary with the machine exactly as the interpreter
would have left it: registers including the halves the code never meant to
use, flags computed by the interpreter's own helpers (`cpu_flags_sub` and
friends in `cpu.h`), memory, the stack. It may stop wherever it likes — after
the function's `retf`, at the head of a loop after k iterations, at the start
of a floating-point tail it would rather leave to the interpreter — and it may
decline, in which case the instruction it was patched over runs instead.

"Exactly as the interpreter would" includes the dead stores a compiled function
makes: the saved registers and locals below the stack pointer, the four words a
`push dx / push ax / push dx / push ax / pop eax / pop ecx` sequence leaves
behind, a scratch register still holding the high half of a product. They cost
a few stores to reproduce and they make correctness a mechanical test rather
than a judgement about what is live, which is the whole point.

Two switches keep this honest:

- `--no-native` patches nothing. It is the A/B, and the way to rule the
  routines out when something is wrong.
- `--verify-native N` runs, every Nth time a site is hit, both the routine and
  the code it replaced from the same state, and requires the interpreter to
  arrive at the routine's stopping point with the same registers, flags and
  committed arena. The same state at the same address means the same future,
  so this is what correctness means here, and it needs no per-routine
  description of what is touched. A loop routine that stops at its own head
  after k iterations is matched on the interpreter's kth pass; the comparison
  is made after every step and memory is compared only once the registers
  agree. A mismatch prints both states and stops the machine.

The benchmark is `tools/bench.ps1` (`make bench`): N generated turns with
`--fixed-clock`, timed, and every output file compared byte for byte against
a blessed run of the unpatched emulator. A routine that is wrong in a way the
verifier does not sample would still have to change a turn file to matter,
and this is the check that it did not.

## The sites

| site | name | what it is | share of instructions (10 turns / 50 turns) |
|---|---|---|---|
| seg9:1940 | `rand_long` | random generator, returns `long` | 5.7% / 4.2% |
| seg9:1652 | `rand_mod` | random generator, returns `int` in `[0, n)` | – / 13.5% |
| seg8:2A01 | `scan_loop` | the nearest-object scan's loop body | 15% / 15.5% |
| seg10:490E | `habitability` | planet habitability, with the runtime's sqrt and _ftol inline | 19% / 9% |
| seg2:5916 | `tech_check` | six fields against a race's requirements | 4.1% / 2.3% |
| seg29:222C | `byte_at_3e` | a ten-instruction byte accessor | 1.5% / 1.0% |
| seg37:0DC2 | `sqrt` | the C runtime's sqrt, positive normal argument | 3% / 1.5% |
| seg37:0E40 | `ftol` | the C runtime's double-to-long | 2.8% / 1.4% |

### seg9:1940 and seg9:1652 — the random generator

Two copies of L'Ecuyer's combined multiplicative congruential generator
(CACM 31(6), 1988): `s1 = 40014 s1 mod 2147483563`, `s2 = 40692 s2 mod
2147483399`, result `s1 − s2`. The compiler's rendering of Schrage's method,
per seed:

```
mov eax,[s]
mov ecx,0xffff2e5c      ; -53668, the quotient's negative
cdq
idiv ecx                ; q = s / -53668
mov ecx,0x7fffffab      ; 2147483563
imul ecx
mov edx,eax
shr edx,0x10
mov si,ax
mov di,dx               ; di:si = low32(q * m)
mov eax,[s]
mov ecx,0x9c4e          ; 40014
imul ecx
mov edx,eax
shr edx,0x10
add si,ax
adc di,dx               ; di:si += low32(s * a)
mov [bp-8],si
mov [bp-6],di
or di,di
jge +8
add [bp-8],0x7fffffab   ; if negative, += m
```

which is `40014 (s mod 53668) − 12211 q` exactly, since
`2147483563 = 53668 × 40014 + 12211` and the true value fits in 32 bits.

`rand_long` (seeds at DGROUP:2292 and 2296) returns `s1 − s2` in DX:AX
through `mov eax,[bp-8]; mov dx,[bp-6]; mov ecx,[bp-4]; mov bx,[bp-2]; sub
ax,cx; sbb dx,bx`, which is why the routine leaves ECX holding the new s2 and
BX its high word, and the flags of the `sbb`.

`rand_mod` (seeds at DGROUP:12D8 and 12DC) takes an `int n` at `[bp+6]`,
forms `r = s1 − s2`, adds `2147483562` if `r < 1`, stores the seeds, and
returns 0 in AX if `n <= 0` — leaving EAX's high word as the high word of s2,
EDX as the high word of the last product and ECX as 40692, with the flags of
the `xor ax,ax` — or else `r mod n` (unsigned `div ecx`) in DX:AX with ECX
holding n sign-extended and the flags of the `shr edx,16` that formed DX.

Both stop after the `retf`. The frame they must leave: the saved BP, DI and
SI, the two seeds as locals after their fix-ups, `rand_mod`'s `r` at
`[bp-C]` and, on the `n > 0` path, the `push eax` of n below the saved SI.

### seg8:2A01 — the nearest-object scan

Inside seg8:2988, which is called ten thousand times per ten turns and runs
twelve thousand instructions per call, this loop walks an array of `(x, y)`
word pairs at DS:SI, one per object, comparing each against `(x0, y0)` at
`[bp+6]` and `[bp+8]`:

```
2A01 mov ax,[bp+8]
     sub ax,[si+2]
     mov [bp-a],ax            ; dy
     mov ax,[bp+6]
     sub ax,[si]              ; dx
     cwd
     push dx / push ax / push dx / push ax
     pop eax / pop ecx        ; eax = ecx = (long) dx
     imul ecx                 ; eax = dx²
     mov [bp-4],eax
     cmp eax,[bp-12]          ; against the best squared distance so far
     jg 2A72                  ; farther already: next
2A25 ...                      ; add dy², compare again, maybe record a new best
2A72 add si,4
     inc di
     cmp di,[7a]              ; the object count
     jl 2A01
2A7C ...
```

Six million iterations in ten turns take the `jg`; the update path at 2A25
runs a few thousand times. So the routine, patched at the head, runs
iterations for as long as they skip, and stops either at 2A7C with the
`cmp di,[7a]` in the flags or back at 2A01 with the registers of the last
iteration it took — EAX the square, ECX the sign-extended dx, EDX zero, SI and
DI advanced, `[bp-a]` and `[bp-4]` written, the four pushed words below SP.
If the very first iteration is one it would not take, it declines and
changes nothing; the interpreter then runs that iteration, update path and
all, and comes back to the head. Flags are dead at 2A01 (the first
instruction to read them is the `jg`, after a `cmp`), but the routine sets
them anyway, to the `cmp` that brought the guest here, because the verifier
compares the register whole.

### seg10:490E — habitability, the integer part

`int hab(planet far *p, int race)`, called 640,000 times in ten turns. For
each of three axes it reads the planet's value at `es:p[C + i]` and the
race's centre, low and high for that axis — bytes at `DS:59D2`, `59D5` and
`59D8` plus `race × C0 + i` — and does:

```
high < 0                immune:      sum += 10000
low <= value <= high    in range:    pct = 100 |value − centre| / half-width
                                     sum += (100 − pct)²
                                     excess = 2 |value − centre| − half-width
                                     if (excess > 0)
                                         factor = factor × (2 half-width − excess)
                                                / (2 half-width)     (32-bit, truncating)
otherwise               out of range: red += min(distance outside, 15)
```

Then, `red != 0`: return `−red`. `red == 0`: a floating-point tail, which
97% of calls take:

```
4A4E push 2710             ; dword
     push [bp-12]          ; factor
     fild dword [bp-C]     ; sum
     fmul qword [1D02]     ; 1/3
     call far sqrt         ; seg37:0DC2
     fadd qword [1D0A]     ; 0.9, so the truncation below rounds
     call far _ftol        ; seg37:0E40, DX:AX
     push dx / push ax / pop eax
     pop ecx               ; factor
     imul ecx
     pop ecx               ; 10000
     cdq
     idiv ecx              ; the product's low half over 10000
     mov edx,eax
     shr edx,10
     mov [bp-C],ax
     pop si / pop di / leave / retf
```

The routine does the integer part and then, when `sum > 0` (so the value
sqrt sees is a positive normal, 1/3 being one), the tail as well, with the
two runtime calls performed inline by the same bodies the `sqrt` and `ftol`
routines below use.
Otherwise it stops at 4A4E for the interpreter to finish, and on the negative
path it stops after the `retf`.

The half-widths are `centre − low` and `high − centre`, and each is an `idiv`
divisor; a zero one makes the routine decline the whole call rather than
guess how the guest's division fault would present. The registers are tracked
through every path as the code moves them, 32-bit halves included, because a
`pop eax` or `imul ecx` on one axis leaves a value the next axis's 16-bit moves
do not clear, and the caller gets that value back in ECX or EDX. The locals
`[bp-2]`, `[bp-6]` and `[bp-18]`, and the push/pop scratch below SP, are
written only on the paths that write them, so a slot the guest never touched
keeps whatever it held.

### seg2:5916 — six fields against a race's requirements

`int check(char far *have)`: six signed bytes at ES:DI against six at
`DS:59DC + race × C0` (the race from DS:14C), with a nibble at DS:59FB naming
the one field allowed to fall short:

```
for (i = 0; i < 6; i++)
    if (req[i] < have[i]) {
        count++;
        if ((nibble & 0xF) == i) {
            if (req[i] - have[i] + 1 == 0) flag = 1;
            else idx = i + 1;
        }
    }
count == 0          -> 1
count == 1 && flag  -> 2
count == 1 && idx   -> have[idx-1] - req[idx-1] + 1
otherwise           -> 99
```

Stops after the `retf`. BX comes back as `count`, or as `DI + idx − 1` on the
third path; CX as the last sign-extended byte moved into it; the flags are
those of whichever `cmp` or `inc` decided the return.

### seg29:222C — a byte accessor

`int f(int a, int b)`: `return (signed char) DS:[a + b + 3E]`. Ten
instructions and over a million calls per ten turns, so the call costs more
than the body. It touches no flags.

### seg37:0DC2 and seg37:0E40 — the C runtime's sqrt and _ftol

Both are x87 code, and the only way to be bit-exact with the interpreter is
to perform the same host operations in the same order under the same guest
control word. `fpu.h` exports the primitives `fpu_exec` is made of for this —
`fpu_get`, `fpu_set`, `fpu_load`, `fpu_pop`, `fpu_arith`, `fpu_sqrt`,
`fpu_xam`, `fpu_to_int`, `fpu_store_f64`, `fpu_clex` — and the routines do no
floating-point arithmetic in C at all. Each is written as a body (the effects
between the far call and the `retf`) with a site routine around it, so the
habitability tail can run the bodies where the guest calls the functions.

`_ftol`:

```
0E40 mov ax,ds / nop / inc bp / push bp / mov bp,sp / push ds / mov ds,ax
     sub sp,C / push bx / push cx / push si / push di
     fnstcw [bp-4]
     mov ax,[bp-4]
     or ah,0C               ; round toward zero
     mov [bp-6],ax
     fldcw [bp-6]
     fistp qword [bp-E]
     fldcw [bp-4]
     mov ax,[bp-E]
     mov dx,[bp-C]          ; the low 32 bits of the 64-bit result
     pop di / pop si / pop cx / pop bx / lea sp,[bp-2] / pop ds / pop bp
     dec bp
     retf
```

The frame is below the stack pointer after the return and so is dead; what
survives is DX:AX, the popped x87 stack, the control word restored, the
status word as `fistp` left it, and the flags of the `dec bp` with the CF of
the `or` before it. It declines if the x87 stack is empty.

`sqrt` takes the runtime's generic path for the one-argument functions:

```
0DC2 mov dx,1858           ; this function's descriptor in DGROUP
     jmp 1C8E
1C8E (the far-frame prologue, sub sp,12)
     cmp byte [1BC8],0      ; the coprocessor found yet?
     jz 1CAC                ; not yet: save the argument at 1A14 first
     call 20C2              ; classify and dispatch:
       fnstcw [bp-6]
       mov bl,[bp-6] / or bl,38 / mov bh,13 / mov [bp-8],bx
       fldcw [bp-8]         ; extended precision, round to nearest, PE/UE/OE masked
       fxam
       fnstsw [bp-A]
       (rotate C3..C0 into an index, xlat through DS:1A5D, cbw)
       and cx,404           ; C1, the sign, into CL
       mov bx,dx / add bx,ax / add bx,10
       jmp [bx]             ; a positive normal lands on:
12DC   or cl,cl / jnz 12EE  ; negative? no
       fsqrt
       ret
1CA5 mov byte [1A44],1
     mov al,[bp-11] / or al,al / jg 1D2D
     fnclex
     fst qword [16A6]       ; the result
     fnstsw [bp-A]
     test word [bp-A],8 / jnz 1D29    ; overflow?
     cmp al,6 / jz 1D31
     cmp byte [1A44],0 / jnz 1D1C
1D1C fldcw [bp-6]
     lea sp,[bp-2] / pop ds / pop bp / dec bp / retf
```

Seventy-six instructions for the case that matters (seventy-nine with the
argument store, before the runtime has found the coprocessor). The routine
performs exactly that sequence: the double stores to DS:1A14 (when taken) and
DS:16A6, the byte at DS:1A44, the runtime's control word for the `fxam` and
`fsqrt` and the caller's put back after, the `fnclex`, and AX 0, BX 1868, CL 0
with CH masked to its bit 2, DX 1858 on the way out. It declines for any
other class of argument — zero, negative, infinite, NaN, an empty stack —
since those go through other handlers, and for an argument at or above
2^1000, where storing it or its root as a double could set the overflow the
epilogue tests.

## Measured

The benchmark game, `--fixed-clock`, output identical to the unpatched
emulator's in every file. The interpreted count is what the interpreter still
executes; the native count is what the routines stood in for; the time is
the interpreter's own from the first instruction to the last.

| build | turns | interpreted | native | time | per turn |
|---|---|---|---|---|---|
| `--no-native` | 10 | 839 M | – | 8.36 s | 0.84 s |
| four routines (the generators, the scan, habitability's integer part) | 10 | 526 M | 314 M | 5.70 s | 0.57 s |
| six (plus the tech check and the accessor) | 10 | 481 M | 361 M | 5.15 s | 0.51 s |
| eight (plus sqrt, _ftol and the habitability tail) | 10 | 394 M | 448 M | 4.42 s | 0.44 s |
| `--no-native` | 50 | 9,714 M | – | 89.7 s | 1.79 s |
| four routines | 50 | 5,775 M | 3,968 M | 57.1 s | 1.14 s |
| six | 50 | 5,469 M | 4,287 M | 53.8 s | 1.08 s |
| eight | 50 | 4,963 M | 4,793 M | 49.4 s | 0.99 s |

The verifier's cost is in the sampling: `--verify-native 97` over ten turns
takes about a minute, most of it copying the committed arena three times per
sample. Every routine above passed it at that rate, and at one in 997 over
fifty turns.

What is left, on ten turns, is spread thinner: the per-player loops in
seg29:3404 and seg15:5F00/64FC (the latter 8% of a fifty-turn run), seg2:5194,
seg8:01E4, seg15:0688 — each 1–3%. The interpreter's own per-instruction cost
is now the larger share, which is the argument for the generic work (a
predecoded instruction cache, data-segment base caching) rather than for the
next site.
