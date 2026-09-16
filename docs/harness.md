# The LLM harness: driving the game from outside

`make harness` builds `StarsVM-harness.exe`, which is the emulator with a
named-pipe control channel compiled in.  `tools/starsmcp.py` is the other end:
run it as an MCP stdio server and it launches the game itself, or use
`--cli --attach` against one already running.  `.mcp.json` registers it.

None of it is in a release.  `harness.h` declares the hooks as empty inline
functions unless `STARSVM_HARNESS` is defined, and `src/unity_harness.c` is
what defines it, so `StarsVM.exe` is byte-for-byte what it was.  That property
is worth checking when a hook is added, and it is easy to break by accident:
passing `HDC_32(hdc)` to a hook grew the release binary by 128 bytes, because
the lookup is a real call whose effects the optimiser cannot discard even when
the function it feeds compiles to nothing.  Hooks that only have the guest's
own handle take the 16-bit handle and convert on the harness side.

## The windows, and what they are really called

The class names do not all say what the pane is.

| class | what it is |
|---|---|
| `starsframe` | the main window; menu commands go here |
| `starsscan` | the scanner: the star map |
| `starsplanet` | the **Command** pane, left column |
| `starsmine` | the **Summary** pane, the wide strip along the bottom |
| `starsmessage` | the messages pane |
| `starstb` | the toolbar |
| `starspopup` | the explanatory popup a summary bar puts up |
| `starsreport` | a report window, e.g. the fleet summary |

The Command pane shows what you can *command*.  A foreign planet never appears
there however it is selected - it goes to the Summary pane instead - so
"the pane did not change" is not by itself a failure.

The tutor is an ordinary `#32770` whose title carries the page number from page
two onward (`Stars! Tutor - Page 17 of 80`), and it is destroyed and remade for
every page, so neither its handle nor its exact title can be held on to.

## Reading the game

`TEXT` returns what a window drew, and it works because the emulator records
every `TextOut`, `ExtTextOut` and `DrawText`.  That covers the tutor's prose,
the messages, the command and summary panes, the reports, and the popups.

`MAP` returns the same text with coordinates, plus the primitives.  It is how
anything without a control is located: a star is a 3x3 blit, and the blue
diamond in the waypoint task tile is a stack of one-pixel bars, which is enough
to find it and right-click it.  What is *not* recorded is colour, so "the green
Radiation bar" and "the big green Shaggy Dog" have to be taken on trust.

Both report `recorded`, which distinguishes a window that drew nothing from one
the harness has no record for - a plain dialog built from standard controls is
the honest `false`, because the guest never paints it.

## Reaching things on the map

The scanner has no scrollbars, and `Find` is the game's own answer:

* `stars_find` - View > Find, which scrolls until the named object is on screen
  and reports its id and universe coordinates in the scanner's status line.
  It does **not** move the Command pane.
* `stars_select` - the same, and then a click at the point those universe
  coordinates map to, which does move the Command pane.  The mapping is fitted
  from the stars currently drawn against `stars_dump universe`, so it follows
  any zoom or scroll; on the tutorial's 24 planets it places an object to
  within six pixels.
* `stars_zoom` - View > Zoom.  Below 50% the game draws no star names at all,
  and `stars_stars` then finds nothing.
* `stars_dump` - Report > Dump to Text File, read back: the universe's
  coordinates, or what is known of planets or fleets.  Much cheaper than
  walking the panes, and it is the player's own menu item.

## Driving it

* **`KEY` is for keys that are not characters** - the arrows, the function
  keys.  `TYPE` is for letters.  A posted `WM_KEYDOWN` is also translated to a
  `WM_CHAR` by the guest's own loop and this game acts on both, so `KEY n`
  advances two fleets and `TYPE n` advances one.
* **Everything is posted, so everything needs settling.**  A click lands on the
  next turn of the guest's message loop, and the pane it updates may be a turn
  behind that.  `stars_wait_idle` is the right instrument; a fixed sleep under
  about three seconds will intermittently read the previous state and look like
  the click missed.
* **Some gestures are the holding of the button, not the clicking of it.**
  `stars_click_at` posts the down and the up together and the guest runs
  neither until the command returns, so it cannot express a button that is
  held.  The summary pane's bars are the case that matters: they put up their
  explanatory popup while the button is down and only act on the release, and
  a plain click on one leaves the game having never seen the gesture finish.
  Use `stars_press`, settle, `stars_release`.  The release is delivered to
  whatever took the mouse capture, which here is the popup rather than the
  pane, so aiming it back at the pane would be wrong even with the timing
  right.
* **A real drag has to be walked.**  `stars_drag` posts its whole path in one
  call, and since none of the guest runs until that call returns, a game that
  follows the cursor rather than the messages sees only the final position.
  The ship designer is the case that matters: dragging a part onto the hull
  does nothing at all that way.  Walk it instead - `stars_press`, several
  `stars_mouse` moves, `stars_release` - with a moment between each.
* **A press must be aimed at the control it starts on.**  A posted message is
  not hit-tested into child windows the way a real click is, so naming the
  dialog and giving a point that happens to lie over its list box puts the
  press on the dialog and the list never sees it.  Press the list; the moves
  and the release then go to whatever took the capture, which is usually the
  dialog, so give those in the dialog's own coordinates.
* **Modifiers are explicit.**  A click sets them and leaves them set - it has
  to, because the click is read after the command returns - so `KEY` and
  `CLICK` take their own, and setting them to zero is how you get an unmodified
  press after a shift-click.  `stars_click` takes `shift`, which is what the
  production queue's Add button reads to add ten at a time.
* **Popup menus block the game, and not only right-clicks open them.**
  Right-click, then `stars_menu_items`, then `stars_menu_pick` - one click, and
  the game is stopped inside `TrackPopupMenu` the whole time; `stars_menu_items`
  reports `waiting` when it is.  A *left* click opens one too: clicking a column
  heading in a report is how its sort is chosen.  So a click that appears to do
  nothing may have opened a menu that nobody answered - always look.
* **Message boxes are answered for you** with the default, and recorded.  Poll
  `stars_msgbox`: the tutorial's corrections ("you have given the fleet the
  wrong destination") arrive only that way.

## Smaller things worth knowing

* **A dialog's handles go stale when it is reopened.**  The game destroys and
  remakes these dialogs, so a handle cached across a close-and-reopen names a
  dead window and every click on it silently does nothing.  Re-observe after
  anything that could have closed a dialog.  This cost half an hour on the
  merge dialog, where the clicks looked fine and the fleets never merged.
* **A multi-select list box's selection is invisible.**  `LIST` reports the
  focused item, not the set, so the merge dialog's list reads the same whether
  nothing or everything is chosen; the asterisks in its text mean something
  else entirely.  Drive it with the keyboard - arrows then space, aimed at the
  list - and confirm by the outcome or a screenshot, not by reading it back.
* **Gauges are set by where the pointer is, not by how far it moved.**  The
  cargo, fuel and warp gauges all take an absolute position, so the way to hit
  an exact value is to press, release and read, and bisect: the fuel gauge
  reached exactly 383mg in seven steps that way.
* **Reports open too small to use.**  The planet report is 600 pixels wide and
  draws columns out past 1400, and its scrollbar is in the non-client area
  where an injected click cannot go.  Resize it with `stars_move` and give a
  width; at 1500 all fifteen columns are reachable.
* **A column heading opens a menu, and the menu has submenus.**  Clicking one
  is how a report's sort is chosen, and several of the entries are submenus -
  "Reverse Sort by Min Conc > Weighted Average" is one leaf of one of them.

## Things that are known to be rough

* Colour is not captured, so instructions that name one cannot be checked.
* `MoveTo`/`LineTo`, `StretchDIBits`, `FillRect`, `FrameRect` and `DrawIcon`
  are not recorded, and `WindowFromDC` gives nothing for a memory DC, so
  anything double-buffered would be invisible.
* The harness only runs when the guest reaches its message loop, so it is
  unresponsive for as long as a turn takes to generate.  The tutorial's game is
  small enough that this is under two seconds; a large game would not be.
