/* dos.h - the DOS/file side, shared between api_dos.c and api_res.c. */
#ifndef DOS_H
#define DOS_H

#include <stdint.h>
#include <wchar.h>

/* Open `path` and seek to `offset`, returning a DOS file handle (0xFFFF on
   failure).  AccessResource needs this to hand the guest a handle positioned at
   a resource, which is how the game reads the bitmaps that exceed 64 KB. */
uint16_t dos_open_at(const wchar_t *path, uint32_t offset);

#endif
