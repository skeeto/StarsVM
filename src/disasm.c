/* disasm.c - a compact 16-bit disassembler for tracing.
 *
 * This exists to make an instruction trace readable, not to be complete: it
 * decodes lengths correctly for everything the interpreter executes (so a trace
 * stays in sync) and names the common instructions.  Anything it does not know
 * is printed as raw bytes rather than guessed at.
 */

#include "cpu.h"
#include "sel.h"
#include "native.h"

#include <stdio.h>
#include <string.h>

static const char *r16n[8] = { "ax","cx","dx","bx","sp","bp","si","di" };
static const char *r8n[8]  = { "al","cl","dl","bl","ah","ch","dh","bh" };
static const char *r32n[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
static const char *segn[6] = { "es","cs","ss","ds","fs","gs" };
static const char *alun[8] = { "add","or","adc","sbb","and","sub","xor","cmp" };
static const char *shfn[8] = { "rol","ror","rcl","rcr","shl","shr","shl","sar" };
static const char *ccn[16] = { "o","no","b","ae","z","nz","be","a",
                               "s","ns","p","np","l","ge","le","g" };

struct ds {
    uint16_t sel, off, start;
    int      osize;
    const char *segpfx;
};

static uint8_t  d8(struct ds *d)  { uint8_t v = sel_rd8(d->sel, d->off); d->off++; return v; }
static uint16_t d16(struct ds *d) { uint16_t v = sel_rd16(d->sel, d->off); d->off = (uint16_t)(d->off + 2); return v; }
static uint32_t d32(struct ds *d) { uint32_t v = sel_rd32(d->sel, d->off); d->off = (uint16_t)(d->off + 4); return v; }

static const char *regname(int r, int size)
{
    return size == 1 ? r8n[r] : size == 2 ? r16n[r] : r32n[r];
}

/* Decode a 16-bit ModRM into text; returns the reg field. */
static int modrm(struct ds *d, char *out, size_t n, int size)
{
    uint8_t m = d8(d);
    int mod = m >> 6, rm = m & 7, reg = (m >> 3) & 7;
    const char *base[8] = { "bx+si","bx+di","bp+si","bp+di","si","di","bp","bx" };
    char disp[24] = "";

    if (mod == 3) {
        snprintf(out, n, "%s", regname(rm, size));
        return reg;
    }
    if (mod == 1) {
        int8_t v = (int8_t)d8(d);
        snprintf(disp, sizeof disp, "%c0x%x", v < 0 ? '-' : '+',
                 v < 0 ? -(int)v : (int)v);
    } else if (mod == 2) {
        snprintf(disp, sizeof disp, "+0x%x", d16(d));
    }
    if (mod == 0 && rm == 6)
        snprintf(out, n, "%s[0x%x]", d->segpfx, d16(d));
    else
        snprintf(out, n, "%s[%s%s]", d->segpfx, base[rm], disp);
    return reg;
}

int disasm(uint16_t sel, uint16_t off, char *out, int len)
{
    struct ds d;
    uint8_t op;
    char rm[48], txt[96];
    int rep = 0;

    d.sel = sel; d.off = off; d.start = off;
    d.osize = 2; d.segpfx = "";
    txt[0] = 0;

    for (;;) {
        op = d8(&d);
        if (op == 0x66) { d.osize = (d.osize == 2) ? 4 : 2; continue; }
        if (op == 0x67) continue;
        if (op == 0x26) { d.segpfx = "es:"; continue; }
        if (op == 0x2E) { d.segpfx = "cs:"; continue; }
        if (op == 0x36) { d.segpfx = "ss:"; continue; }
        if (op == 0x3E) { d.segpfx = "ds:"; continue; }
        if (op == 0xF0) continue;
        if (op == 0xF2) { rep = 2; continue; }
        if (op == 0xF3) { rep = 3; continue; }
        break;
    }

    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int size = (op & 1) ? d.osize : 1;
        int reg = modrm(&d, rm, sizeof rm, size);
        if (op & 2) snprintf(txt, sizeof txt, "%s %s,%s", alun[(op >> 3) & 7],
                             regname(reg, size), rm);
        else        snprintf(txt, sizeof txt, "%s %s,%s", alun[(op >> 3) & 7],
                             rm, regname(reg, size));
        break;
    }
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C:
        snprintf(txt, sizeof txt, "%s al,0x%x", alun[(op >> 3) & 7], d8(&d));
        break;
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D:
        snprintf(txt, sizeof txt, "%s %s,0x%x", alun[(op >> 3) & 7],
                 regname(0, d.osize), d.osize == 2 ? d16(&d) : d32(&d));
        break;
    case 0x80: case 0x81: case 0x83: {
        int size = (op == 0x80) ? 1 : d.osize;
        int reg = modrm(&d, rm, sizeof rm, size);
        uint32_t imm = (op == 0x80) ? d8(&d)
                     : (op == 0x83) ? (uint32_t)(int8_t)d8(&d)
                     : (size == 2 ? d16(&d) : d32(&d));
        snprintf(txt, sizeof txt, "%s %s,0x%x", alun[reg], rm, imm);
        break;
    }
    case 0x84: case 0x85: {
        int size = (op & 1) ? d.osize : 1;
        int reg = modrm(&d, rm, sizeof rm, size);
        snprintf(txt, sizeof txt, "test %s,%s", rm, regname(reg, size));
        break;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        int size = (op & 1) ? d.osize : 1;
        int reg = modrm(&d, rm, sizeof rm, size);
        if (op & 2) snprintf(txt, sizeof txt, "mov %s,%s", regname(reg, size), rm);
        else        snprintf(txt, sizeof txt, "mov %s,%s", rm, regname(reg, size));
        break;
    }
    case 0x8C: { int reg = modrm(&d, rm, sizeof rm, 2);
                 snprintf(txt, sizeof txt, "mov %s,%s", rm, segn[reg]); break; }
    case 0x8E: { int reg = modrm(&d, rm, sizeof rm, 2);
                 snprintf(txt, sizeof txt, "mov %s,%s", segn[reg], rm); break; }
    case 0x8D: { int reg = modrm(&d, rm, sizeof rm, d.osize);
                 snprintf(txt, sizeof txt, "lea %s,%s", regname(reg, d.osize), rm); break; }
    case 0xC4: case 0xC5: { int reg = modrm(&d, rm, sizeof rm, 2);
                 snprintf(txt, sizeof txt, "%s %s,%s", op == 0xC4 ? "les" : "lds",
                          regname(reg, 2), rm); break; }
    case 0xC6: case 0xC7: {
        int size = (op & 1) ? d.osize : 1;
        modrm(&d, rm, sizeof rm, size);
        snprintf(txt, sizeof txt, "mov %s,0x%x", rm,
                 size == 1 ? d8(&d) : (size == 2 ? d16(&d) : d32(&d)));
        break;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        snprintf(txt, sizeof txt, "mov %s,0x%x", r8n[op & 7], d8(&d));
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        snprintf(txt, sizeof txt, "mov %s,0x%x", regname(op & 7, d.osize),
                 d.osize == 2 ? d16(&d) : d32(&d));
        break;
    case 0xA0: case 0xA1:
        snprintf(txt, sizeof txt, "mov %s,%s[0x%x]",
                 regname(0, (op & 1) ? d.osize : 1), d.segpfx, d16(&d));
        break;
    case 0xA2: case 0xA3:
        snprintf(txt, sizeof txt, "mov %s[0x%x],%s", d.segpfx, d16(&d),
                 regname(0, (op & 1) ? d.osize : 1));
        break;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        snprintf(txt, sizeof txt, "push %s", regname(op & 7, d.osize)); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        snprintf(txt, sizeof txt, "pop %s", regname(op & 7, d.osize)); break;
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        snprintf(txt, sizeof txt, "push %s", segn[(op >> 3) & 3]); break;
    case 0x07: case 0x17: case 0x1F:
        snprintf(txt, sizeof txt, "pop %s", segn[(op >> 3) & 3]); break;
    case 0x68: snprintf(txt, sizeof txt, "push 0x%x",
                        d.osize == 2 ? d16(&d) : d32(&d)); break;
    case 0x6A: snprintf(txt, sizeof txt, "push 0x%x", (uint32_t)(int8_t)d8(&d)); break;
    case 0x60: snprintf(txt, sizeof txt, "pusha"); break;
    case 0x61: snprintf(txt, sizeof txt, "popa"); break;
    case 0x9C: snprintf(txt, sizeof txt, "pushf"); break;
    case 0x9D: snprintf(txt, sizeof txt, "popf"); break;
    case 0x9E: snprintf(txt, sizeof txt, "sahf"); break;
    case 0x9F: snprintf(txt, sizeof txt, "lahf"); break;
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        snprintf(txt, sizeof txt, "inc %s", regname(op & 7, d.osize)); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        snprintf(txt, sizeof txt, "dec %s", regname(op & 7, d.osize)); break;
    case 0x86: case 0x87: {
        int size = (op & 1) ? d.osize : 1;
        int reg = modrm(&d, rm, sizeof rm, size);
        snprintf(txt, sizeof txt, "xchg %s,%s", rm, regname(reg, size));
        break;
    }
    case 0x90: snprintf(txt, sizeof txt, "nop"); break;
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97:
        snprintf(txt, sizeof txt, "xchg %s,%s", regname(0, d.osize),
                 regname(op & 7, d.osize)); break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int size = (op & 1) ? d.osize : 1;
        int reg = modrm(&d, rm, sizeof rm, size);
        if (op == 0xC0 || op == 0xC1)
            snprintf(txt, sizeof txt, "%s %s,0x%x", shfn[reg], rm, d8(&d));
        else if (op == 0xD0 || op == 0xD1)
            snprintf(txt, sizeof txt, "%s %s,1", shfn[reg], rm);
        else
            snprintf(txt, sizeof txt, "%s %s,cl", shfn[reg], rm);
        break;
    }
    case 0xF6: case 0xF7: {
        static const char *g3[8] = { "test","test","not","neg","mul","imul","div","idiv" };
        int size = (op & 1) ? d.osize : 1;
        int reg = modrm(&d, rm, sizeof rm, size);
        if (reg < 2)
            snprintf(txt, sizeof txt, "test %s,0x%x", rm,
                     size == 1 ? d8(&d) : (size == 2 ? d16(&d) : d32(&d)));
        else
            snprintf(txt, sizeof txt, "%s %s", g3[reg], rm);
        break;
    }
    case 0xFE: case 0xFF: {
        static const char *g5[8] = { "inc","dec","call","call far","jmp","jmp far","push","?" };
        int size = (op == 0xFE) ? 1 : d.osize;
        int reg = modrm(&d, rm, sizeof rm, size);
        snprintf(txt, sizeof txt, "%s %s", g5[reg], rm);
        break;
    }
    case 0xE8: { int16_t r = (int16_t)d16(&d);
                 snprintf(txt, sizeof txt, "call 0x%x", (uint16_t)(d.off + r)); break; }
    case 0xE9: { int16_t r = (int16_t)d16(&d);
                 snprintf(txt, sizeof txt, "jmp 0x%x", (uint16_t)(d.off + r)); break; }
    case 0xEB: { int8_t r = (int8_t)d8(&d);
                 snprintf(txt, sizeof txt, "jmp 0x%x", (uint16_t)(d.off + r)); break; }
    case 0x9A: { uint16_t o = d16(&d), s = d16(&d);
                 snprintf(txt, sizeof txt, "call far %04X:%04X", s, o); break; }
    case 0xEA: { uint16_t o = d16(&d), s = d16(&d);
                 snprintf(txt, sizeof txt, "jmp far %04X:%04X", s, o); break; }
    case 0xC3: snprintf(txt, sizeof txt, "ret"); break;
    case 0xC2: snprintf(txt, sizeof txt, "ret 0x%x", d16(&d)); break;
    case 0xCB: snprintf(txt, sizeof txt, "retf"); break;
    case 0xCA: snprintf(txt, sizeof txt, "retf 0x%x", d16(&d)); break;
    case 0xCF: snprintf(txt, sizeof txt, "iret"); break;
    case 0xC8: { uint16_t a = d16(&d); uint8_t l = d8(&d);
                 snprintf(txt, sizeof txt, "enter 0x%x,%u", a, l); break; }
    case 0xC9: snprintf(txt, sizeof txt, "leave"); break;
    case 0xCD: snprintf(txt, sizeof txt, "int 0x%x", d8(&d)); break;
    case 0xCC: snprintf(txt, sizeof txt, "int3"); break;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t r = (int8_t)d8(&d);
        snprintf(txt, sizeof txt, "j%s 0x%x", ccn[op & 15], (uint16_t)(d.off + r));
        break;
    }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        static const char *ln[4] = { "loopnz","loopz","loop","jcxz" };
        int8_t r = (int8_t)d8(&d);
        snprintf(txt, sizeof txt, "%s 0x%x", ln[op & 3], (uint16_t)(d.off + r));
        break;
    }
    case 0x69: case 0x6B: {
        int reg = modrm(&d, rm, sizeof rm, d.osize);
        uint32_t imm = (op == 0x6B) ? (uint32_t)(int8_t)d8(&d)
                     : (d.osize == 2 ? d16(&d) : d32(&d));
        snprintf(txt, sizeof txt, "imul %s,%s,0x%x", regname(reg, d.osize), rm, imm);
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD:
    case 0xAE: case 0xAF: {
        static const char *sn[] = { "movs","cmps","stos","lods","scas" };
        int which = (op < 0xA6) ? 0 : (op < 0xAA) ? 1 : (op < 0xAC) ? 2
                  : (op < 0xAE) ? 3 : 4;
        snprintf(txt, sizeof txt, "%s%s%c",
                 rep == 3 ? "rep " : rep == 2 ? "repne " : "",
                 sn[which], (op & 1) ? (d.osize == 2 ? 'w' : 'd') : 'b');
        break;
    }
    case 0xA8: snprintf(txt, sizeof txt, "test al,0x%x", d8(&d)); break;
    case 0xA9: snprintf(txt, sizeof txt, "test %s,0x%x", regname(0, d.osize),
                        d.osize == 2 ? d16(&d) : d32(&d)); break;
    case 0x98: snprintf(txt, sizeof txt, d.osize == 2 ? "cbw" : "cwde"); break;
    case 0x99: snprintf(txt, sizeof txt, d.osize == 2 ? "cwd" : "cdq"); break;
    case 0xD7: snprintf(txt, sizeof txt, "xlat"); break;
    case 0xF5: snprintf(txt, sizeof txt, "cmc"); break;
    case 0xF8: snprintf(txt, sizeof txt, "clc"); break;
    case 0xF9: snprintf(txt, sizeof txt, "stc"); break;
    case 0xFA: snprintf(txt, sizeof txt, "cli"); break;
    case 0xFB: snprintf(txt, sizeof txt, "sti"); break;
    case 0xFC: snprintf(txt, sizeof txt, "cld"); break;
    case 0xFD: snprintf(txt, sizeof txt, "std"); break;
    case 0x9B: snprintf(txt, sizeof txt, "fwait"); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        modrm(&d, rm, sizeof rm, 2);
        snprintf(txt, sizeof txt, "esc %02X %s", op, rm);
        break;
    }
    case 0x0F: {
        uint8_t o2 = d8(&d);
        if (o2 >= 0x80 && o2 <= 0x8F) {
            int16_t r = (int16_t)d16(&d);
            snprintf(txt, sizeof txt, "j%s 0x%x", ccn[o2 & 15], (uint16_t)(d.off + r));
        } else if (o2 >= 0x90 && o2 <= 0x9F) {
            modrm(&d, rm, sizeof rm, 1);
            snprintf(txt, sizeof txt, "set%s %s", ccn[o2 & 15], rm);
        } else if (o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF) {
            int ssz = (o2 & 1) ? 2 : 1;
            int reg = modrm(&d, rm, sizeof rm, ssz);
            snprintf(txt, sizeof txt, "mov%cx %s,%s", o2 < 0xBE ? 'z' : 's',
                     regname(reg, d.osize), rm);
        } else if (o2 == 0xAF) {
            int reg = modrm(&d, rm, sizeof rm, d.osize);
            snprintf(txt, sizeof txt, "imul %s,%s", regname(reg, d.osize), rm);
        } else {
            snprintf(txt, sizeof txt, "(0f %02x)", o2);
        }
        break;
    }
    case 0xD6: {                                  /* a native routine's site */
        const char *nm = native_name_at(sel, off);
        if (nm) snprintf(txt, sizeof txt, "native %s", nm);
        else    snprintf(txt, sizeof txt, "(d6)");
        break;
    }
    default:
        snprintf(txt, sizeof txt, "(%02x)", op);
        break;
    }

    {
        int n = 0, i, ilen = (int)(uint16_t)(d.off - d.start);
        n += snprintf(out + n, (size_t)(len - n), "%04X:%04X ", sel, d.start);
        for (i = 0; i < ilen && i < 8; i++)
            n += snprintf(out + n, (size_t)(len - n), "%02X",
                          sel_rd8(sel, (uint16_t)(d.start + i)));
        for (; i < 8; i++)
            n += snprintf(out + n, (size_t)(len - n), "  ");
        snprintf(out + n, (size_t)(len - n), " %s", txt);
        return ilen;
    }
}
