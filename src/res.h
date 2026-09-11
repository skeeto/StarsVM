/* res.h - reaching into the module's own resources.
 *
 * FindResource/LoadResource hand the guest a far pointer to a copy.  Dialogs,
 * menus and icons are different: we read the resource ourselves and build a
 * Win32 object from it, so what is wanted is a pointer straight into the file
 * image and a length.
 */
#ifndef RES_H
#define RES_H

#include <stdint.h>

/* Win16 resource type ids, as they appear in the NE resource table. */
#define RT16_CURSOR       0x8001
#define RT16_BITMAP       0x8002
#define RT16_ICON         0x8003
#define RT16_MENU         0x8004
#define RT16_DIALOG       0x8005
#define RT16_STRING       0x8006
#define RT16_ACCELERATOR  0x8009
#define RT16_GROUP_CURSOR 0x800C
#define RT16_GROUP_ICON   0x800E

/* Resolve a guest resource-name argument - a string, a "#123", or a
   MAKEINTRESOURCE far pointer with a zero selector - and return a pointer into
   the loaded file image, with the length in *len.  NULL if there is no such
   resource. */
const uint8_t *res_locate(uint16_t type_id, uint32_t name_segptr, uint32_t *len);

/* The same, given a name already in host memory. */
const uint8_t *res_locate_name(uint16_t type_id, const char *name, uint32_t *len);

/* The same, for a resource already known by its numeric id. */
const uint8_t *res_locate_id(uint16_t type_id, uint16_t id, uint32_t *len);

/* Resolve a string-named resource TYPE - "WAVE", say - to the id the NE
   resource table uses for it, which is the byte offset of its Pascal string
   within that table.  That id is what the res_locate_* calls above want.
   Returns 0 if the module has no such type. */
uint16_t res_type_key(const char *type_name);

#endif
