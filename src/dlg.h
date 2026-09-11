/* dlg.h - the dialog layer's two hooks into the rest of the shim. */
#ifndef DLG_H
#define DLG_H

#include <stdint.h>
#include <windows.h>

void api_dlg_register(void);

/* A dialog's DLGPROC, for SetWindowLong/GetWindowLong(DWL_DLGPROC).  Returns 0
   if the window is not one of our dialogs, which is also how offset 4 is told
   apart from an ordinary window's extra long at offset 4. */
uint32_t dlg_proc_get(HWND h);
void     dlg_proc_set(HWND h, uint32_t proc16, uint16_t hinst);

#endif
