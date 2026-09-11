/* task.h - the Win16 task: PSP, instance data, and the startup contract. */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <wchar.h>
#include "ne.h"
#include "cpu.h"

/* INSTANCEDATA, which always sits at offset 0 of DGROUP.  Win16 code reads
   these directly, so the offsets are ABI. */
#define ID_NULL        0x00   /* always zero, so a null near pointer reads as 0 */
#define ID_OLD_SP      0x02
#define ID_OLD_SS      0x04
#define ID_HEAP        0x06   /* near pointer to the local heap info */
#define ID_ATOMTABLE   0x08
#define ID_STACKTOP    0x0A   /* lowest legal SP */
#define ID_STACKMIN    0x0C
#define ID_STACKBOTTOM 0x0E   /* initial SP */
#define ID_SIZE        0x10

/* PSP fields we fill in. */
#define PSP_INT20      0x00
#define PSP_DISPATCH   0x05   /* far call to DOS3Call, for `call PSP:0005` */
#define PSP_PARENT     0x16
#define PSP_ENVIRON    0x2C
#define PSP_CMDLINE    0x80   /* length byte then the text */
#define PSP_SIZE       0x110

typedef struct {
    NeModule *mod;
    uint16_t  hinstance;      /* DGROUP selector, which is also hInstance */
    uint16_t  hmodule;        /* module handle handed to the guest          */
    uint16_t  psp_sel;
    uint16_t  env_sel;
    uint16_t  stacktop;
    int       ncmdshow;
    char      cmdline[128];
    /* The guest sees these, so they stay in its own byte encoding: they go into
       its environment and come back out of GetModuleFileName.  They are also
       what its file dialogs and its own file calls are built from. */
    char      exepath[520];   /* full path of the module file               */
    char      exedir[520];    /* the directory holding it                   */
    /* The same two as Windows really spells them.  A path we open OURSELVES -
       Stars.ini, and the module file when AccessResource reopens it - has to go
       through these, or a directory name outside the ANSI code page turns into
       question marks and nothing opens. */
    wchar_t   exepathw[520];
    wchar_t   exedirw[520];
} Task;

extern Task task;

/* Build the PSP, environment and instance data, then set the registers to the
   Win16 entry-point contract and point the CPU at the entry point. */
int task_start(NeModule *m, Cpu *c, const char *cmdline, int ncmdshow);

#endif
