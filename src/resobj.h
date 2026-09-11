/* resobj.h - Win32 menus, icons and cursors built from the module's own
   Win16 resources.  See resobj.c. */
#ifndef RESOBJ_H
#define RESOBJ_H

#include <windows.h>

/* Build a menu from the named RT_MENU resource.  NULL if there is none. */
HMENU menu_load16(const char *name);

/* Build an icon (is_icon) or cursor (!is_icon) from the named RT_GROUP_ICON or
   RT_GROUP_CURSOR resource.  NULL if there is none. */
HICON icon_load16(const char *name, int is_icon);

/* An icon from the module's resources at a specific size. */
HICON icon_load16_sized(const char *name, int cx, int cy);

/* The module's own application icon, cached, at the requested size. */
HICON icon_app16(int cx, int cy);

#endif
