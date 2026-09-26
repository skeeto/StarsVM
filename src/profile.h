/* profile.h - what Stars.ini says when it says nothing.
 *
 * Shared by the emulator's INI (api_profile.c), which falls back to these when
 * the file has no answer, and the library's (headless.c), which has no file
 * and so always does.
 */
#ifndef PROFILE_H
#define PROFILE_H

/* When disabled, supply a fixed GlobalSettings for Stars.ini */
#define GLOBAL_SETTINGS_PRESET "cXK3c0vpLLSpdAgeAMJdjUcWXpnp"  // EGGSWAIN

/* Window Layout: 0 large, 1 medium, 2 small.  Asked with nothing in the file to
   answer from, the game says medium, which is a 1995 reading of a roomy screen
   - 800x600.  Every screen is large by that standard now, so a first run gets
   the large layout instead.

   A default, not an override: the game writes the layout back to Stars.ini on
   the way out, so the moment a player picks one from the Window Layout menu
   their choice is in the file and this never applies again. */
#define LAYOUT_LARGE 0

#endif
