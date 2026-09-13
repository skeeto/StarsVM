/* main.c - run Stars! 2.70j by emulating Win16 on top of Win32. */

#include "ne.h"
#include "sel.h"
#include "log.h"
#include "cpu.h"
#include "thunk.h"
#include "fpu.h"
#include "task.h"
#include "audio.h"
#include "hostclock.h"
#include "prof.h"
#include "native.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

void imp_dump_table(void);
int  call16_init(void);
void api_kernel_register(void);
void api_dos_register(void);
void api_user_register(void);
void api_gdi_register(void);
void api_res_register(void);
void api_misc_register(void);
void api_profile_register(void);
void api_dlg_register(void);
extern int thunk_survey;
extern int trace_paint;

static NeModule module;

/* What to call ourselves in messages.  Taken from argv[0] rather than written
   down, because the appended-module trick means the finished program is meant
   to be renamed - Stars-x86.exe, in the README - so any name compiled in here
   is wrong for exactly the build this project recommends.  The name was written
   down in nine PowerShell tools once too, and every one of them went on
   reporting that "stars16" was not running long after nothing built under that
   name; tools/starsproc.ps1 exists to keep that from happening again. */
static const char *me = "Stars!VM";

static void set_me(const char *argv0)
{
    const char *p;
    if (!argv0) return;
    for (p = argv0; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') argv0 = p + 1;
    if (*argv0) me = argv0;
}

static const char usage_text[] =
    "\n"
    "With no path, the game is taken from a module appended to this executable\n"
    "if there is one - compressed or not - and otherwise from the stars.exe\n"
    "beside it.  `make onefile` builds the appended, compressed form, which is a\n"
    "single self-contained program; nothing else needs installing.\n"
    "\n"
    "  --module PATH   load this module, ignoring any appended one\n"
    "  --dump          print the NE structure and exit\n"
    "  --dump-relocs   as --dump, with per-segment relocation counts\n"
    "  --imports       print the import thunk table and exit\n"
    "  --load          load and relocate the module, then report\n"
    "  --peek S:OFF:N  after loading, hex-dump N bytes at segment S offset OFF\n"
    "  --disasm S:OFF:N\n"
    "                  after loading, disassemble N instructions from there\n"
    "  --run           load and start executing at the entry point (default)\n"
    "  --steps N       stop after N instructions in the outer loop, 0 for\n"
    "                  no limit (the default).  Guest callbacks are never\n"
    "                  bounded, so most of a GUI run is not counted\n"
    "  --trace-api     log every Win16 call\n"
    "  --trace-cpu N   log the first N instructions executed\n"
    "  --survey        keep going past unimplemented APIs (returning 0)\n"
    "  --console       open a console for the log (this is a GUI binary)\n"
    "  --fixed-clock   pin every clock the guest can read, so that a run\n"
    "                  writes byte-identical save files given the same\n"
    "                  input - which is what makes a turn comparable\n"
    "  --trace-paint   log update regions around painting (repaint loops)\n"
    "  --no-native     interpret everything: patch in none of the native\n"
    "                  routines that stand in for the game's hottest code\n"
    "                  (docs/natives.md).  The A/B for them, and the way to\n"
    "                  rule them out\n"
    "  --verify-native N\n"
    "                  every Nth time a native routine runs, also run the\n"
    "                  guest code it replaced from the same state and stop\n"
    "                  on any difference\n"
    "  --play-wave N   play \"WAVE\" resource N through the sound path\n"
    "                  and exit (the game has 2601 2602 2611 2612 2621 2631;\n"
    "                  N = 0 plays all six, overlapping)\n"
    "  --log FILE      also write the log to FILE\n"
    "  --help          this text\n"
    "\n"
    "Everything starting with `-` before the `--` is ours, so the game's own\n"
    "switches go after it; a bare filename may come either side.  A player,\n"
    "host or custom race file opens straight into the game, with no splash\n"
    "screen.  The switches Stars! parses for itself:\n"
    "\n"
    "  -a DEF          create a new game from a .def file\n"
    "  -b LIST         generate turns for every game named in LIST, then exit\n"
    "  -d[pfm] FILE    dump a player's planets, fleets and/or map, then exit\n"
    "  -g[N] HOST      generate N turns (one by default) from a host file and\n"
    "                  exit - the batch mode, and with --fixed-clock the way\n"
    "                  to get a run that can be compared against another\n"
    "  -h              ask for a password every time a turn file is opened\n"
    "  -m              start with the music off\n"
    "  -p PASSWORD     give the password instead of being asked for it\n"
    "  -s              start with the battle sound effects off\n"
    "  -t FILE         try, then exit: open the new turn if the host has made\n"
    "                  one, or with a host file make it if every player is in\n"
    "  -w [HOST]       wait: with a host file generate each turn as soon as\n"
    "                  the players are all in, with a player file wait for the\n"
    "                  host to do it.  Does not exit\n"
    "  -x              exit Windows when the game does - nothing here.  It\n"
    "                  asks 16-bit Windows to shut the machine down, which we\n"
    "                  decline and treat as the quit it was already part of\n"
    "\n"
    "The game's help file describes them at starsfaq.com/command.htm.\n";

/* The help, put somewhere it can be read - which for a GUI binary takes doing.
   A shell gets it through its own console, borrowed for the purpose.  Started
   from the Run box or a shortcut there is no console to borrow, so one is made
   and held open until the reader is done: this is a page of text, and a page in
   a message box is a page whose last third is off the bottom of the screen.  A
   message box is still the last resort, as it is for a run that fails. */
static void show_help(void)
{
    static char text[sizeof usage_text + 256];

    snprintf(text, sizeof text,
             "%s - run the 16-bit Stars! under a Win16-to-Win32 shim\n"
             "\n"
             "usage: %s [options] [stars.exe] [-- game arguments]\n"
             "%s", me, me, usage_text);
    switch (log_adopt_console(1)) {
    case CON_ALREADY:
        fputs(text, stdout);
        fflush(stdout);
        break;
    case CON_MADE:
        fputs(text, stdout);
        fputs("\n[press Enter to close]\n", stdout);
        fflush(stdout);
        getchar();
        break;
    default:
        MessageBoxA(NULL, text, me, MB_OK | MB_ICONINFORMATION);
        break;
    }
}

/* Our own full path.  Wide throughout: everything we go on to open ourselves -
   the module, and the Stars.ini beside it - is built from this, and
   GetModuleFileNameA would already have spelled a directory outside the ANSI
   code page as question marks. */
static int self_path(wchar_t *out, size_t n)
{
    DWORD r = GetModuleFileNameW(NULL, out, (DWORD)n);
    return r > 0 && r < n;
}

/* argv reached us through the ANSI code page, which cannot spell every path
   Windows can.  Flags are ASCII and fine; the one argument naming a file we
   open ourselves is taken from the wide command line instead.  The game gets no
   such treatment, and can get none: its arguments reach it through a PSP
   command tail, which is bytes. */
static void wide_arg(int argi, const char *narrow, wchar_t *out, size_t n)
{
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

    out[0] = 0;
    if (wargv && argi > 0 && argi < wargc)
        _snwprintf(out, n - 1, L"%ls", wargv[argi]);
    else
        MultiByteToWideChar(CP_ACP, 0, narrow, -1, out, (int)n);
    out[n - 1] = 0;
    if (wargv) LocalFree(wargv);
}

/* Default to the stars.exe sitting beside us. */
static void default_target(wchar_t *out, size_t n)
{
    wchar_t self[MAX_PATH];
    wchar_t *slash;

    if (!self_path(self, sizeof self / sizeof *self)) {
        _snwprintf(out, n - 1, L"stars.exe");
        out[n - 1] = 0;
        return;
    }
    slash = wcsrchr(self, L'\\');
    if (slash) *slash = 0;
    _snwprintf(out, n - 1, L"%ls\\stars.exe", self);
    out[n - 1] = 0;
}

/* What the run cost.  The instruction count is the interpreter's own and the
   clock is the host's, so the rate is the one number that says whether a change
   to the interpreter helped. */
static void report_stop(int r, double secs)
{
    log_msg("\nStopped: %s after %llu instructions in %.3f s (%.1f M/s)\n",
            cpu_state_name(r), (unsigned long long)cpu.icount, secs,
            secs > 0.0 ? (double)cpu.icount / secs / 1e6 : 0.0);
    /* Each native call counted as one instruction above.  This is what they
       stood in for, so the two lines together say what a full interpretation
       would have cost. */
    if (native_calls)
        log_msg("  native: %llu calls stood in for %llu more instructions\n",
                (unsigned long long)native_calls,
                (unsigned long long)native_instrs);
}

int main(int argc, char **argv)
{
    const char *modopt = NULL;
    const char *logfile = NULL;
    wchar_t targetw[MAX_PATH * 2];
    int modopti = 0, opened = 0;
    /* Everything bound for the game, in the order it was written.  A bare
       filename is a candidate for the module path too, and which one - if any -
       becomes the module is not known until the appended module has been tried,
       so the decision waits and this keeps the order meanwhile. */
    struct { const char *s; int argi, bare; } garg[32];
    int ngarg = 0, endopt = 0;
    char gcmd[127];             /* a PSP tail is a length byte and 126 more */
    int do_dump = 0, do_imports = 0, do_load = 0, do_run = 0, verbose = 0;
    uint64_t steps = 0;
    long trace_cpu = 0;
    long play_wave = -1;
    struct { unsigned seg, off, len, code; } peek[8];
    int npeek = 0;
    int i;
    LARGE_INTEGER qfreq, qt0, qt1;

    set_me(argv[0]);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (endopt || a[0] != '-') {
            /* The game's.  A module candidate only while bare: past `--` the
               intent was stated, so it is not second-guessed. */
            if (ngarg == (int)(sizeof garg / sizeof *garg)) {
                fprintf(stderr, "%s: too many arguments\n", me);
                return 2;
            }
            garg[ngarg].s = a;
            garg[ngarg].argi = i;
            garg[ngarg].bare = !endopt;
            ngarg++;
        } else if (!strcmp(a, "--")) {
            endopt = 1;
        } else if (!strcmp(a, "--module") && i + 1 < argc) {
            modopt = argv[++i];
            modopti = i;
        } else if ((!strcmp(a, "--peek") || !strcmp(a, "--disasm")) &&
                   i + 1 < argc && npeek < 8) {
            peek[npeek].code = a[2] == 'd';
            if (sscanf(argv[++i], "%u:%x:%u", &peek[npeek].seg,
                       &peek[npeek].off, &peek[npeek].len) == 3) {
                npeek++;
                do_load = 1;
            } else {
                fprintf(stderr, "%s: %s wants SEG:HEXOFF:LEN\n", me, a);
                return 2;
            }
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            show_help();
            return 0;
        } else if (!strcmp(a, "--dump")) {
            do_dump = 1;
        } else if (!strcmp(a, "--dump-relocs")) {
            do_dump = 1; verbose = 1;
        } else if (!strcmp(a, "--imports")) {
            do_imports = 1;
        } else if (!strcmp(a, "--load")) {
            do_load = 1;
        } else if (!strcmp(a, "--run")) {
            do_run = 1;
        } else if (!strcmp(a, "--play-wave") && i + 1 < argc) {
            play_wave = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--trace-paint")) {
            trace_paint = 1;
        } else if (!strcmp(a, "--survey")) {
            thunk_survey = 1;
        } else if (!strcmp(a, "--no-native")) {
            native_disable();
        } else if (!strcmp(a, "--verify-native") && i + 1 < argc) {
            native_verify((unsigned)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(a, "--console")) {
            log_console = 1;
        } else if (!strcmp(a, "--fixed-clock")) {
            clock_fixed = 1;
        } else if (!strcmp(a, "--steps") && i + 1 < argc) {
            steps = (uint64_t)_strtoui64(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--trace-cpu") && i + 1 < argc) {
            trace_cpu = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--trace-api")) {
            log_verbose = 1;
        } else if (!strcmp(a, "--log") && i + 1 < argc) {
            logfile = argv[++i];
        } else {
            fprintf(stderr, "%s: unknown option %s\n", me, a);
            return 2;
        }
    }

    targetw[0] = 0;

    /* Double-clicked, or run with nothing but a path: play the game.  The
       inspection modes are what needs asking for, not the ordinary one. */
    if (!do_dump && !do_imports && !do_load && npeek == 0)
        do_run = 1;

    /* --play-wave wants the module loaded and a task, because the waves are
       resources inside it - but not the interpreter. */
    if (play_wave >= 0) do_run = 1;

    /* An inspection mode is all text and exits at once, so it has --help's
       problem and takes --help's answer: borrow the shell's console.  A run of
       the game does not, for the reason log_open gives. */
    if (!do_run) log_adopt_console(0);

    log_open(logfile);

    if (!sel_init()) return 1;
    if (!thunk_init()) return 1;
    if (!call16_init()) return 1;

    if (do_imports) {
        imp_dump_table();
        if (!do_dump && !do_load && !do_run) { log_close(); return 0; }
    }

    /* Four places the game can be, in order of how deliberate they are.
       --module says so outright.  Otherwise a module appended to this
       executable wins, so that
           cat StarsVM.exe stars.exe > Stars-x86.exe
       is a single self-contained program with nothing else to install - and so
       that a bare filename handed to the packed build is a game file for the
       game to open, not a module for us to load.  Failing that the first bare
       argument is the module, which is how `StarsVM.exe stars.exe` has always
       worked.  Failing that, the stars.exe sitting beside us. */
    if (modopt) {
        wide_arg(modopti, modopt, targetw, sizeof targetw / sizeof *targetw);
        opened = ne_open(&module, targetw);
    } else {
        wchar_t self[MAX_PATH];
        opened = self_path(self, sizeof self / sizeof *self) &&
                 ne_open_appended(&module, self);
        if (!opened) {
            int k;
            for (k = 0; k < ngarg; k++)
                if (garg[k].bare) {
                    wide_arg(garg[k].argi, garg[k].s, targetw,
                             sizeof targetw / sizeof *targetw);
                    garg[k].s = NULL;      /* ours, so not the game's */
                    break;
                }
            if (!targetw[0])
                default_target(targetw, sizeof targetw / sizeof *targetw);
            opened = ne_open(&module, targetw);
        }
    }
    if (!opened) {
        log_msg("%s: cannot read %s\n", me, log_wide(targetw));
        log_close();
        return 1;
    }

    if (do_dump) {
        ne_dump(&module, verbose);
        if (!do_load && !do_run) { ne_close(&module); log_close(); return 0; }
    }

    if (!do_load && !do_run) {
        log_msg("%s: nothing to do - try --dump, --imports, --load or --run\n", me);
        ne_close(&module);
        log_close();
        return 0;
    }

    if (!ne_load(&module, thunk_resolve, NULL)) {
        log_msg("%s: load failed\n", me);
        ne_close(&module);
        log_close();
        return 1;
    }
    log_msg("Loaded %u segments; DGROUP selector %04X, entry %04X:%04X\n",
            module.cseg, module.dgroup_sel,
            ne_seg(&module, (unsigned)(module.csip >> 16))->sel,
            (unsigned)(module.csip & 0xFFFF));

    for (i = 0; i < npeek; i++) {
        unsigned segno = peek[i].seg, off = peek[i].off, len = peek[i].len, j;
        NeSeg *s = ne_seg(&module, segno);
        if (!s) { log_msg("peek: no segment %u\n", segno); continue; }
        if (peek[i].code) {
            /* N instructions, in the form the profiler and the traces use. */
            char line[160];
            for (j = 0; j < len; j++) {
                int n = disasm(s->sel, (uint16_t)off, line, sizeof line);
                log_msg("seg%u:%s\n", segno, line + 5);
                if (n <= 0) break;
                off += (unsigned)n;
            }
            continue;
        }
        log_msg("seg %u (%04X):%04X ", segno, s->sel, off);
        for (j = 0; j < len; j++)
            log_msg("%02X ", sel_rd8(s->sel, (uint16_t)(off + j)));
        log_msg("\n");
    }

    if (!do_run) {
        thunk_report_unbound();
        ne_close(&module);
        log_close();
        return 0;
    }

    /* ---- execute ---- */
    api_kernel_register();
    api_dos_register();
    api_user_register();
    api_gdi_register();
    api_res_register();
    api_misc_register();
    api_audio_register();
    api_profile_register();
    api_dlg_register();
    thunk_report_unbound();
    /* The game's own command line, in the order it was written, less whatever
       became the module path.  A real command tail begins with the separator,
       so this one does too.  build_psp would quietly cut an over-long tail at
       126 bytes, and that would surface as the game mis-reading a path rather
       than as anything to do with us, so refuse to build one instead. */
    gcmd[0] = 0;
    {
        size_t n = 0;
        int k;
        for (k = 0; k < ngarg; k++) {
            size_t len;
            if (!garg[k].s) continue;
            len = strlen(garg[k].s);
            if (n + 1 + len >= sizeof gcmd) {
                log_msg("%s: the game needs %u bytes of command line and a PSP"
                        " tail holds 126 - run from the game directory so the"
                        " paths can be relative\n",
                        me, (unsigned)(n + 1 + len));
                gcmd[0] = 0;
                break;
            }
            gcmd[n++] = ' ';
            memcpy(gcmd + n, garg[k].s, len);
            n += len;
            gcmd[n] = 0;
        }
    }
    if (*gcmd) log_msg("Game command line:%s\n", gcmd);

    /* Before the first instruction and before prof_begin takes its copy of the
       code, so that the profiler's immutability check sees the patched code as
       the code. */
    native_install(&module);

    if (!task_start(&module, &cpu, gcmd, 1)) {
        ne_close(&module);
        log_close();
        return 1;
    }
    /* The effects are reachable only from the battle VCR, so being able to
       drive the sound path without one is what makes it testable at all. */
    if (play_wave >= 0) {
        int rc = audio_selftest((unsigned)play_wave);
        ne_close(&module);
        log_close();
        return rc;
    }

    log_msg("\nStarting at %04X:%04X, ss:sp %04X:%04X, ds %04X\n\n",
            cpu.seg[S_CS], (unsigned)cpu.eip, cpu.seg[S_SS],
            reg16(&cpu, R_SP), cpu.seg[S_DS]);

    prof_begin(&module);
    fpu_host_enter();
    QueryPerformanceFrequency(&qfreq);
    QueryPerformanceCounter(&qt0);
    if (trace_cpu > 0) {
        char line[160];
        long n = 0;
        int r = CPU_RUNNING;
        cpu.state = CPU_RUNNING;
        while (r == CPU_RUNNING) {
            if (n < trace_cpu) {
                disasm(cpu.seg[S_CS], (uint16_t)cpu.eip, line, sizeof line);
                log_msg("%s\n", line);
            }
            r = cpu_step(&cpu);
            if (steps && ++n >= (long)steps) { r = CPU_STEPS; break; }
            if (n < trace_cpu) continue;
            if (trace_cpu && n == trace_cpu)
                log_msg("... trace limit reached, continuing quietly\n");
        }
        QueryPerformanceCounter(&qt1);
        report_stop(r, (double)(qt1.QuadPart - qt0.QuadPart) /
                       (double)qfreq.QuadPart);
    } else {
        int r = cpu_run(&cpu, steps);
        QueryPerformanceCounter(&qt1);
        report_stop(r, (double)(qt1.QuadPart - qt0.QuadPart) /
                       (double)qfreq.QuadPart);
    }
    fpu_host_leave();
    prof_report();
    audio_shutdown();

    if (cpu.state == CPU_NOAPI) {
        log_msg("  That API has no implementation yet; the call is named above.\n"
                "  Use --survey to keep going past these and collect the whole\n"
                "  list of what is still missing.\n");
    }

    /* Started from Explorer with no --log and no --console, the report above
       went nowhere.  A failure has to say so somehow, or the program simply
       vanishes - which is exactly the behaviour this phase set out to end. */
    if (!log_visible() && cpu.state != CPU_HALT) {
        char msg[512];
        const char *missing = thunk_last_missing();
        int n = snprintf(msg, sizeof msg, "Stars! stopped: %s",
                         cpu_state_name(cpu.state));
        if (cpu.state == CPU_NOAPI && missing)
            n += snprintf(msg + n, sizeof msg - n,
                          "\n\n%s has no implementation yet.", missing);
        else if (cpu.state == CPU_BADOP || cpu.state == CPU_FAULT)
            n += snprintf(msg + n, sizeof msg - n,
                          "\n\nat %04X:%04X, opcode %02X %02X",
                          (unsigned)cpu.bad_cs, (unsigned)cpu.bad_ip,
                          cpu.bad_op, cpu.bad_op2);
        snprintf(msg + n, sizeof msg - n,
                 "\n\nRe-run with --log FILE (or --console) for the details.");
        MessageBoxA(NULL, msg, me, MB_OK | MB_ICONERROR);
    }
    if (cpu.state == CPU_BADOP || cpu.state == CPU_FAULT) {
        char line[160];
        log_msg("  at %04X:%04X, opcode %02X %02X\n",
                cpu.bad_cs, cpu.bad_ip, cpu.bad_op, cpu.bad_op2);
        disasm((uint16_t)cpu.bad_cs, (uint16_t)cpu.bad_ip, line, sizeof line);
        log_msg("  %s\n", line);
        log_msg("  ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X\n",
                reg16(&cpu, R_AX), reg16(&cpu, R_BX), reg16(&cpu, R_CX),
                reg16(&cpu, R_DX), reg16(&cpu, R_SI), reg16(&cpu, R_DI),
                reg16(&cpu, R_BP), reg16(&cpu, R_SP));
        log_msg("  ds=%04X es=%04X ss=%04X flags=%04X\n",
                cpu.seg[S_DS], cpu.seg[S_ES], cpu.seg[S_SS],
                (unsigned)(cpu.eflags & 0xFFFF));
    }

    ne_close(&module);
    log_close();
    return 0;
}
