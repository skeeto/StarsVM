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
* **Modifiers are explicit.**  A click sets them and leaves them set - it has
  to, because the click is read after the command returns - so `KEY` and
  `CLICK` take their own, and setting them to zero is how you get an unmodified
  press after a shift-click.  `stars_click` takes `shift`, which is what the
  production queue's Add button reads to add ten at a time.
* **Popup menus block the game.**  Right-click, then `stars_menu_items`, then
  `stars_menu_pick` - one right-click, and the game is stopped inside
  `TrackPopupMenu` the whole time, so do not dawdle.
* **Message boxes are answered for you** with the default, and recorded.  Poll
  `stars_msgbox`: the tutorial's corrections ("you have given the fleet the
  wrong destination") arrive only that way.

## Things that are known to be rough

* The tutorial's page 28 does not advance, with every step in its text
  performed and verified - the message filtered, the last message read, Wallaby
  selected, the radiation bar's popup read, and the Research dialog opened with
  F5.  Generating is refused with "you have not yet completed all of the
  tutorial tasks for this turn".  Unresolved.
* Colour is not captured, so instructions that name one cannot be checked.
* `MoveTo`/`LineTo`, `StretchDIBits`, `FillRect`, `FrameRect` and `DrawIcon`
  are not recorded, and `WindowFromDC` gives nothing for a memory DC, so
  anything double-buffered would be invisible.
* The harness only runs when the guest reaches its message loop, so it is
  unresponsive for as long as a turn takes to generate.  The tutorial's game is
  small enough that this is under two seconds; a large game would not be.
