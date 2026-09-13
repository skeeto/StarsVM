/* dos.h - the DOS/file side, shared between api_dos.c and api_res.c. */
#ifndef DOS_H
#define DOS_H

#include <stdint.h>
#include <wchar.h>

/* A DOS file handle onto `len` bytes of memory, 0xFFFF if none is free.  The
   span is borrowed, not copied, and must outlive the handle - the only caller
   is AccessResource, handing the guest a handle onto the module image so it can
   _lread the bitmaps that exceed 64 KB.  Read-only and sequential; nothing
   seeks it. */
uint16_t dos_open_mem(const uint8_t *mem, uint32_t len);

/* Flush and close every handle the guest left open.  Called once, as the
   process ends: a real file is written through a buffer, and this is where
   the last of it goes. */
void dos_shutdown(void);

#endif
