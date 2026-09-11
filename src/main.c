/* main.c - stars16: run Stars! 2.70j by emulating Win16 on top of Win32. */

#include "ne.h"
#include "sel.h"
#include "log.h"
#include "cpu.h"
#include "thunk.h"
#include "fpu.h"
#include "task.h"
#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

void imp_dump_table(void);
int  fuzz_main(long rounds, unsigned seed);
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

static const char usage_text[] =
    "stars16 - run the 16-bit Stars! under a Win16-to-Win32 shim\n"
    "\n"
    "usage: stars16 [options] [Stars!.exe]\n"
    "\n"
    "  --dump          print the NE structure and exit\n"
    "  --dump-relocs   as --dump, with per-segment relocation counts\n"
    "  --imports       print the import thunk table and exit\n"
    "  --load          load and relocate the module, then report\n"
    "  --peek S:OFF:N  after loading, hex-dump N bytes at segment S offset OFF\n"
    "  --run           load and start executing at the entry point (default)\n"
    "  --steps N       stop after N instructions (default 0 = no limit)\n"
    "  --trace-api     log every Win16 call\n"
    "  --trace-cpu N   log the first N instructions executed\n"
    "  --survey        keep going past unimplemented APIs (returning 0)\n"
    "  --console       open a console for the log (this is a GUI binary)\n"
    "  --trace-paint   log update regions around painting (repaint loops)\n"
    "  --play-wave N   play \"WAVE\" resource N through the sound path\n"
    "                  and exit (the game has 2601 2602 2611 2612 2621 2631;\n"
    "                  N = 0 plays all six, overlapping)\n"
    "  --log FILE      also write the log to FILE\n"
    "  --help          this text\n";

/* Default to the Stars!.exe next to us, so the emulator can live in emu/ and
   still be run with no arguments. */
static void default_target(char *out, size_t n)
{
    char self[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, self, sizeof self)) {
        snprintf(out, n, "Stars!.exe");
        return;
    }
    slash = strrchr(self, '\\');
    if (slash) *slash = 0;
    snprintf(out, n, "%s\\Stars!.exe", self);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;
    snprintf(out, n, "%s\\..\\Stars!.exe", self);
}

int main(int argc, char **argv)
{
    const char *target = NULL;
    const char *logfile = NULL;
    char defpath[MAX_PATH * 2];
    int do_dump = 0, do_imports = 0, do_load = 0, do_run = 0, verbose = 0;
    uint64_t steps = 0;
    long trace_cpu = 0;
    int do_fuzz = 0;
    long fuzz_rounds = 200000;
    unsigned fuzz_seed = 0;
    long play_wave = -1;
    struct { unsigned seg, off, len; } peek[8];
    int npeek = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--peek") && i + 1 < argc && npeek < 8) {
            if (sscanf(argv[++i], "%u:%x:%u", &peek[npeek].seg,
                       &peek[npeek].off, &peek[npeek].len) == 3) {
                npeek++;
                do_load = 1;
            } else {
                fprintf(stderr, "stars16: --peek wants SEG:HEXOFF:LEN\n");
                return 2;
            }
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            fputs(usage_text, stdout);
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
        } else if (!strcmp(a, "--fuzz")) {
            do_fuzz = 1;
        } else if (!strcmp(a, "--fuzz-seed") && i + 1 < argc) {
            fuzz_seed = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--fuzz-rounds") && i + 1 < argc) {
            fuzz_rounds = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--play-wave") && i + 1 < argc) {
            play_wave = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--trace-paint")) {
            trace_paint = 1;
        } else if (!strcmp(a, "--survey")) {
            thunk_survey = 1;
        } else if (!strcmp(a, "--console")) {
            log_console = 1;
        } else if (!strcmp(a, "--steps") && i + 1 < argc) {
            steps = (uint64_t)_strtoui64(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--trace-cpu") && i + 1 < argc) {
            trace_cpu = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--trace-api")) {
            log_verbose = 1;
        } else if (!strcmp(a, "--log") && i + 1 < argc) {
            logfile = argv[++i];
        } else if (a[0] == '-') {
            fprintf(stderr, "stars16: unknown option %s\n", a);
            return 2;
        } else {
            target = a;
        }
    }

    /* Double-clicked, or run with nothing but a path: play the game.  The
       inspection modes are what needs asking for, not the ordinary one. */
    if (!do_dump && !do_imports && !do_load && !do_fuzz && npeek == 0)
        do_run = 1;

    /* --play-wave wants the module loaded and a task, because the waves are
       resources inside it - but not the interpreter. */
    if (play_wave >= 0) do_run = 1;

    log_open(logfile);

    if (!target) {
        default_target(defpath, sizeof defpath);
        target = defpath;
    }

    if (!sel_init()) return 1;
    if (!thunk_init()) return 1;
    if (!call16_init()) return 1;

    if (do_fuzz) {
        int rc = fuzz_main(fuzz_rounds, fuzz_seed);
        log_close();
        return rc;
    }

    if (do_imports) {
        imp_dump_table();
        if (!do_dump && !do_load && !do_run) { log_close(); return 0; }
    }

    if (!ne_open(&module, target)) {
        log_msg("stars16: cannot read %s\n", target);
        log_close();
        return 1;
    }

    if (do_dump) {
        ne_dump(&module, verbose);
        if (!do_load && !do_run) { ne_close(&module); log_close(); return 0; }
    }

    if (!do_load && !do_run) {
        log_msg("stars16: nothing to do - try --dump, --imports, --load or --run\n");
        ne_close(&module);
        log_close();
        return 0;
    }

    if (!ne_load(&module, thunk_resolve, NULL)) {
        log_msg("stars16: load failed\n");
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
    if (!task_start(&module, &cpu, "", 1)) {
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

    fpu_host_enter();
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
        log_msg("\nStopped: %s after %llu instructions\n",
                cpu_state_name(r), (unsigned long long)cpu.icount);
    } else {
        int r = cpu_run(&cpu, steps);
        log_msg("\nStopped: %s after %llu instructions\n",
                cpu_state_name(r), (unsigned long long)cpu.icount);
    }
    fpu_host_leave();
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
        MessageBoxA(NULL, msg, "stars16", MB_OK | MB_ICONERROR);
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
