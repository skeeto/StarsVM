I read the four files plus `thunk.c`, `handle.c`, `resobj.c`, the accelerator path in `api_res.c`, the scout traces, and the 36 shipped dialog templates (via `emu/tools/dlgdump.py`, read-only, against `Stars!.exe`). Line numbers are as of my read — `dlg.c` and `api_user.c` were being edited by another agent while I worked (mtimes 10:06/10:08), so I quote the code text too.

---

## 1. `EM_SETSEL` is passed through unchanged; Win16 and Win32 disagree on where the two endpoints live

**`src/msg16.c:91-241`** (`marshal_in`) — there is no `case EM_SETSEL:`. The renumbering at `src/msg16.c:25-27` correctly turns Win16 `0x0401` into Win32 `EM_SETSEL 0x00B1` for an `Edit`, and then `msg16_send` (`src/msg16.c:284`) forwards `m.wp`/`m.lp` verbatim.

* Win16 `EM_SETSEL`: `wParam` = "scroll caret into view" flag (0/1); `LOWORD(lParam)` = start, `HIWORD(lParam)` = end.
* Win32 `EM_SETSEL`: `wParam` = start, `lParam` = end.

So `EM_SETSEL(0, MAKELONG(4,9))` reaches USER32 as start=0, end=0x00090004 (589828). Every selection the game sets collapses to "from character 0 (or 1) to the end of the field".

**Player-visible:** the 13 EDIT controls in the shipped templates — Ship & Starbase Designer design-name (`id=2075`), Rename Battle Plan, Find Planet or Fleet, Change Password ×2, Print Map page counts, Serial Number. The normal idiom "put the default name in and select it so the user can type over it" still looks right by accident, but "place the caret at position N" / "select the bad digits" always selects the whole field instead, so the next keystroke wipes the field.

**Confidence: high** on the defect (both layouts are documented and the code has no case for it); **medium** that Stars! calls it — I could not confirm a call in the traces, because none of the scout regions opens a dialog with an edit field.

Note `CB_SETEDITSEL` and `EM_GETSEL` do *not* need translation (identical in both), so this is the only edit-selection message affected. `EM_LINESCROLL` also differs but is dead here: no template carries `ES_MULTILINE`.

---

## 2. `LB_GETSELITEMS` hands USER32 a guest SEGPTR as a host `int*` — Merge Fleets

**`src/msg16.c:91-241`** — no case for `LB_GETSELITEMS` (Win16 `0x0412` → Win32 `0x0191`) or `LB_GETSELCOUNT`. The `out`/`OUT_*` machinery covers strings, RECTs and tab arrays but not this one, and it is *not* on the `refuse` list at `src/msg16.c:223-237`, so it fails silently instead of logging.

`LB_GETSELITEMS`'s `lParam` is a caller-supplied array of **16-bit** ints in Win16 and **32-bit** ints in Win32. The shim passes the raw far pointer through: USER32 writes `count` DWORDs to the numeric value of the SEGPTR (e.g. `0x0147xxxx` ≈ 21 MB into a 32-bit address space).

**This is reachable in ordinary play.** Dumping the templates:

```
DIALOG 82 ... caption='Merge Fleets'
    LISTBOX   id=81   8,16  130x130  style=50A10009
```

`0x0009 = LBS_MULTIPLESEL | LBS_NOTIFY` — the only multi-select list in the game, and the only sane way to read it is `LB_GETSELCOUNT` + `LB_GETSELITEMS`. (`LB_SETSEL`, `LB_GETSEL` and `LB_SELITEMRANGE` are all layout-identical and are fine.)

**Player-visible:** selecting two or more fleets and hitting OK in Merge Fleets either access-violates or scribbles on whatever is mapped at that address. **Confidence: high** that the marshaling is missing and wrong; **medium-high** that Merge Fleets trips it (I inferred the message from the control style, I did not disassemble the handler).

---

## 3. `DefWindowProc`'s 32-bit handle result is re-narrowed as if it were a 16-bit one — WM_CTLCOLOR, live on the main screen

**`src/api_user.c:319-322`**:

```c
if (winproc_original(hwnd, msg, &msg32, &wp32, &lp32)) {
    LRESULT r = winproc_default(hwnd, msg32, wp32, lp32);
    winproc_refresh_struct(hwnd, msg32, wp32, lp32, lp);
    return (uint32_t)r;      /* raw host handle handed back as DX:AX */
}
```

combined with **`src/winproc.c:636-640`**:

```c
LRESULT winproc_ret_handle(int type, uint32_t r)
{
    if ((uint16_t)r < H_FIRST) return (LRESULT)r;
    return (LRESULT)(uintptr_t)h32_quiet(type, (uint16_t)r);
}
```

`msg_to_16` sets `x->ret_handle = H_BRUSH` for all seven `WM_CTLCOLOR*` (`src/winproc.c:489`). When the guest handler just forwards, `u_DefWindowProc` returns USER32's real `HBRUSH` (a full 32-bit pointer) as the guest's `DX:AX`; `winproc_bridge` (`src/winproc.c:674`) then takes the **low word of that pointer** and runs it through `h32_quiet(H_BRUSH, …)`, which yields `NULL` — or, once the handle table has grown past that index, a *different, real* brush of the right type.

This is not hypothetical. Trace, `scout/gameload-region.log`:

```
19:   USER.107 DefWindowProc(009D 0019 008D 0003 00A0)
792:  USER.107 DefWindowProc(009D 0019 008D 0003 00A1)
816:  USER.107 DefWindowProc(009D 0019 008A 0003 009F)
1367: USER.107 DefWindowProc(009D 0019 00A4 0003 009F)
2618: USER.107 DefWindowProc(00AB 0019 008D 0003 00C1)
```

`0x0019` = `WM_CTLCOLOR`, `HIWORD(lParam) = 3 = CTLCOLOR_BTN`, and the children `009F..00A2` are the four real Win32 buttons created at `scout/startup.log:704,707,710,714` (parent `009D`, ids 0..3, `0x50000000`). So the forward-and-return path runs every time those buttons repaint.

Same hole, same shape, in **`src/api_user.c:1055-1058`** (`u_CallWindowProc`'s wrapped-host-proc branch) — a guest that subclasses a stock control and chains to the original proc gets the same un-narrowed handle back.

**Player-visible:** wrong or unpainted background behind the command buttons / static text, and it *changes over a session* because `h32_quiet` starts returning a real-but-wrong brush once the handle table is deep enough. **Confidence: high** on the mechanism (both halves are in front of me and the trace shows the path is hot); **medium** on exactly how it renders, since a `NULL` return from `WM_CTLCOLORBTN` is tolerated more gracefully than one from `WM_CTLCOLORSTATIC`.

The fix belongs on the `u_DefWindowProc` side (`h16(H_BRUSH, r)` for handle-valued messages), not in `winproc_ret_handle`, which is right for values the guest actually produced.

---

## 4. `WM_MENUSELECT` and `WM_MENUCHAR`: the two halves of `lParam` are swapped, and a popup's `wParam` is an index, not an HMENU

**`src/winproc.c:442-450`**:

```c
case WM_MENUSELECT:
    x->wp16 = LOWORD(wp);
    x->lp16 = (uint32_t)MAKELONG(HMENU_16((HMENU)lp), HIWORD(wp));
    break;

case WM_MENUCHAR:
    x->wp16 = LOWORD(wp);
    x->lp16 = (uint32_t)MAKELONG(HMENU_16((HMENU)lp), HIWORD(wp));
    break;
```

Win16 contract for both: **`LOWORD(lParam)` = the menu flags, `HIWORD(lParam)` = the HMENU.** This code puts the HMENU in the low word and the flags in the high word — exactly inverted. (Compare `WM_COMMAND` at `src/winproc.c:426-429`, which really *does* want the handle in the low word; the two cases were written as if they had the same layout, and they don't.)

Second, smaller error in the same case: in Win32 `LOWORD(wParam)` is the *position index* of a submenu when `HIWORD(wParam) & MF_POPUP`; in Win16 `wParam` was the submenu's **HMENU**. A guest that checks `MF_POPUP` and treats `wParam` as a menu handle gets a small integer.

**Player-visible:** any menu-tracking behaviour keyed off these — a status-line hint, a custom mnemonic handler answering `WM_MENUCHAR` — reads flags where it expects a handle and vice versa; `MF_GRAYED`/`MF_POPUP` tests come out of the handle bits, so the branch taken is effectively random.

**Confidence: high** on the code being wrong (Win16 SDK layout and Wine's `WINPROC_MapMsg32ATo16` both put flags in the low word). **Low** that Stars! reads it: `0x011F` never appears in any `DefWindowProc` histogram, but none of the scout regions covers menu navigation, so that's non-evidence rather than evidence of absence. The `WM_INITMENUPOPUP` path next door *is* live (26 `CheckMenuItem` + 17 `EnableMenuItem` calls) and is correct.

---

## 5. `SetWindowLong(GWL_WNDPROC)` will happily install a *wrapped host* procedure as guest code

**`src/api_user.c:1019-1029`**:

```c
if (off == GWL_WNDPROC) {
    uint32_t prev = winproc_get(hwnd);
    WNDPROC host_prev = NULL;
    if (!prev)
        host_prev = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    winproc_set(hwnd, val, task.hinstance);
    SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)winproc_bridge);
    return prev ? prev : winproc_from_host(host_prev);
}
```

`val` is stored unconditionally. `u_CallWindowProc` (`src/api_user.c:1045`) carefully asks `winproc_to_host(proc)` first; `u_SetWindowLong` does not. So the standard un-subclass idiom — `SetWindowLong(h, GWL_WNDPROC, oldProc)` with the value this same function handed out at `src/api_user.c:1003`/`1028` — stores a pointer into `hostproc_sel`, a bare `sel_alloc(0x1000, SK_CODE)` block with no code in it (`src/winproc.c:45-48`). The next message runs `call16_wndproc` into 4 KB of zeroes, burns the 200-million-instruction budget in `src/thunk.c:194`, logs "guest callback ran away", and latches a fault — the whole app stops.

The game does subclass: `scout/gameload-region.log:576-577, 670-674` show `GetWindowLong(… FFFC)` / `SetWindowLong(… FFFC …)` on three windows during File>Open.

**Player-visible:** the app freezes for a long moment and then dies, at whatever point the game tears a subclass back down. **Confidence: high** on the code path; **medium** that Stars! restores a *host* proc — in the three observed cases `winproc_get(hwnd)` is non-zero (they're the game's own classes), so the value round-tripped is a guest SEGPTR and safe. The trap only springs on a stock control.

Cheap guard: `if (winproc_to_host(val)) { SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)winproc_to_host(val)); winproc_forget(hwnd); ... }`.

---

## 6. `msg16_to_32()` is class-blind, and its second branch is unreachable

**`src/winproc.c:192-197`**:

```c
uint32_t msg16_to_32(uint32_t msg)
{
    if (msg >= 0x0400 && msg <= 0x040F) return msg - 0x0310;
    if (msg >= 0x0400 && msg <= 0x042F) return msg - 0x0350;
    return msg;
}
```

The second `if` can never fire for `0x0400..0x040F`, and `LB_*`/`CB_*`/`SBM_*` are not handled at all. `msg16.c:16-38` solved this properly by consulting `GetClassNameA`; this function is the old, broken twin and it still has three callers:

* **`src/api_user.c:371`** (`get_msg16`, feeding `TranslateMessage`/`DispatchMessage`) paired with `msg32_to_16` at **`src/api_user.c:356`** (`put_msg16`, feeding `GetMessage`/`PeekMessage`). The pair is not an involution: a posted `EM_*` (`0x00B0..0x00BF`) or `SBM_*` (`0x00E0..0x00EF`) is written to the guest as `0x0400..0x040F` and read back as `0x00F0..0x00FF` — i.e. it arrives at `DispatchMessage` as a **`BM_*` message**. A posted `LB_*` (`0x0180..0x018F`) round-trips to `BM_*` likewise.
* **`src/api_user.c:325`** and **`src/api_user.c:1057`**, the non-in-flight fallbacks of `DefWindowProc` and `CallWindowProc`.

Compounding it, **`src/api_user.c:358`** truncates: `sel_wr16(sel, off+4, (uint16_t)m->wParam)` — the high word of a queued message's `wParam` is dropped and cannot be recovered by `get_msg16` at line 371.

**Player-visible:** anything the game *posts* to a control (rather than sends) is delivered as a different message entirely. Only 6 `PostMessage` calls appear across all the scout logs, so this is latent rather than hot — and `u_PostMessage` → `msg16_post` converts to the Win32 number before queueing, which is what keeps the common case alive. **Confidence: high** that the function is wrong as written; **low-medium** that a player hits it, because the game posts so little.

---

## 7. `WM_SETTEXT` / `WM_GETTEXT` reach the guest with a raw host pointer in `lParam`

`msg_to_16` (`src/winproc.c:276-535`) has no case for either, so `x->lp16 = (uint32_t)lp` — the host address. Trace evidence, `scout/gameload-region.log:517`:

```
USER.107 DefWindowProc(008B 000C 0000 00DF E8F0)
```

`0x000C` = `WM_SETTEXT`, `lParam = 0x00DFE8F0` — a host stack pointer sitting in a 16-bit guest's `lParam`. It survives only because the guest forwards it and `winproc_original` (`src/api_user.c:319`) substitutes the original 32-bit pair.

The same hole is on the read side: `u_GetWindowText` (`src/api_user.c:615`) calls `GetWindowTextA` on a *same-process* window, which USER32 implements by **sending `WM_GETTEXT`** to `winproc_bridge` — so a guest window procedure that handles `WM_GETTEXT` itself would `lstrcpyn` into selector `0x00DF`, offset `0xE8F0`.

**Player-visible:** nothing today, because Stars! forwards both. It breaks the moment any guest class handles its own text (custom caption, a window that mirrors its title into a panel). **Confidence: high** that it is unmarshaled (trace-confirmed); **low** that it is currently hit.

Same family, also trace-confirmed and genuinely harmless: **`WM_ENTERIDLE`**, `scout/gameload-region.log:1-5`

```
USER.107 DefWindowProc(009D 0121 0000 0026 10EC)
```

`lParam = 0x002610EC` is the modal dialog's **32-bit HWND** delivered to a 16-bit guest. Win16 put a 16-bit HWND in the low word. One `x->lp16 = HWND_16((HWND)lp)` case fixes it.

---

## 8. `WM_COMPAREITEM` is promised by the header comment and absent from the table

**`src/winproc.c:13-15`** states: *"For WM_CREATE, WM_NCCREATE, WM_DRAWITEM and WM_COMPAREITEM the struct in lParam must sit on the 16-bit stack"* — but `msg_to_16` has no `case WM_COMPAREITEM:`. `grep -rn COMPAREITEM *.c` finds only that comment and the outbound `refuse` entry at `src/msg16.c:232`. Inbound, a `COMPAREITEMSTRUCT*` (host address) lands in the guest's `lParam` and the guest reads 18 bytes at a bogus segmented address.

Right now this is **dead**: none of the six LISTBOXes or ten COMBOBOXes in the 36 templates sets `LBS_SORT (0x0002)` or `CBS_SORT (0x0100)` — styles are `0x0151, 0x0051, 0x0051, 0x0009, 0x0001, 0x0001` and `0x0203`/`0x0003`. It fires the first time anyone creates a sorted owner-draw list. `WM_MEASUREITEM`, `WM_DRAWITEM` and `WM_DELETEITEM` are all present and their 14/26/12-byte layouts check out against the Win16 headers. **Confidence: high** it's missing, **high** it's currently unreachable.

---

## 9. Smaller ones, in descending order

* **`WM_GETFONT` has no `ret_handle`** — `msg16.c:160-162` maps the result of an outbound `WM_GETFONT` through `h16(H_FONT, …)`, but `msg_to_16` never sets `x->ret_handle = H_FONT` for the inbound direction (it does handle `WM_SETFONT` at `src/winproc.c:509`). A guest window procedure that answers `WM_GETFONT` with its own 16-bit HFONT hands USER32 the integer `0x4B`. Asymmetric by inspection; low reachability.
* **Combo boxes get no `CBS_NOINTEGRALHEIGHT`.** The listbox accommodation exists twice — `src/api_user.c:242` (`if (!_stricmp(cls, "listbox")) style |= 0x0100;`) and the freshly added `src/dlg.c:149` (`if (p < end && *p == 0x83) istyle |= 0x0100;`) — but neither covers `CBS_NOINTEGRALHEIGHT (0x0400)` on class ordinal `0x85`, and there are ten COMBOBOXes with dropped heights of 55-95 DLU (New Game race picker, the four `id=1054..1058` selectors). Also note `dlg.c:149` matches only the **one-byte ordinal** form; a template naming `"LISTBOX"` as a string would slip past. (All 36 of these templates do use ordinals, so that half is fine today.)
* **`BACK_NCCALC` copies back only `rgrc[0]`** — `src/winproc.c:565-571`. `msg_to_16` hands the guest all three rects when `wParam` is set (`src/winproc.c:352-366`), but a guest that returns `WVR_VALIDRECTS` and fills `rgrc[1]`/`rgrc[2]` has those discarded, so USER32 blits from stale rectangles. Symptom would be smeared client content while dragging a panel edge. Low likelihood (most Win16 code returns 0 here).
* **Handle slots are never reused.** `src/handle.c:27` (`static unsigned next_free = FIRST_HANDLE;`) only ever increments; `h_release` (`src/handle.c:89-101`) clears the entry but not the counter, and it is called for exactly three things (`DestroyWindow`, `DestroyMenu`, `DestroyIcon`/`DestroyCursor`). Every child control, DC, brush, pen and font mapped over a long game adds a slot; at `MAX_HANDLES 8192` (`src/handle.c:9`) `h16` starts returning 0 and every subsequent handle silently becomes NULL. Partly self-limiting because `h16` de-dupes on the host pointer and Win32 recycles HWND/HDC values, so I could not estimate a real time-to-failure. Worth a counter in the log if nothing else.
* **`inflight_push` can be skipped while `inflight_pop` still decrements** — `src/winproc.c:~360-375`. At `MAX_INFLIGHT 32` the push is dropped but the matching pop is not, so the stack top drifts and a later push overwrites a live outer frame; `winproc_original` could then hand `DefWindowProc` another message's parameters. In practice `call16_wndproc` latches a fatal fault at the same depth (`src/thunk.c:135-141`), so the window is narrow. Cheap fix: make `inflight_push` return whether it pushed and have `winproc_call16` pop only then.

---

## Things I checked and found correct (so nobody re-walks them)

`WM_COMMAND`, `WM_HSCROLL`/`WM_VSCROLL`, `WM_ACTIVATE`, `WM_PARENTNOTIFY` (trace-verified: `DefWindowProc(009D 0210 0001 0003 00A2)` has the child id in the high word, as Win16 wants), `WM_VKEYTOITEM`/`WM_CHARTOITEM`, the seven-into-one `WM_CTLCOLOR` fan-in and its `CTLCOLOR_*` ordering, the `CREATESTRUCT16`/`MEASUREITEMSTRUCT16`/`DRAWITEMSTRUCT16`/`DELETEITEMSTRUCT16` field offsets, `MINMAXINFO` point order, the `msg16_to_32_for` per-class offsets (I re-derived all five tables entry by entry — `BM +0x0310`, `EM +0x0350`, `LB +0x0281`, `CB +0x02C0`, `SBM +0x0320`, `STM +0x0290` — and every bound is right), `SetWindowPos`'s `HWND_TOP/BOTTOM/TOPMOST/NOTOPMOST` special-casing, `TrackPopupMenu`'s argument order and reserved param, `menu_walk`'s `MF_END` masking and separator detection (`src/resobj.c:58-71`), the accelerator table conversion and its `0x80` last-entry mask (`src/api_res.c:391-397`), the Win32 `DLGTEMPLATE`/`DLGITEMTEMPLATE` field order and 4-byte alignment, and the `GWL_*` negative offsets (Win32 kept Win16's values, so the passthrough at `src/api_user.c:997`/`1035` is right).