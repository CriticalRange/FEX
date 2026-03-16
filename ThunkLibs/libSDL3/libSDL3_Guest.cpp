#include <SDL3/SDL.h>

#include "common/Guest.h"
#include "thunkgen_guest_libSDL3.inl"

static void libSDL3_guest_init() {
  // VEXA_FIXES: Ensure SDL runtime marks main as initialized for non-SDL
  // entry paths (guest main() not routed through SDL's usual bootstrap).
  SDL_SetMainReady();
}

LOAD_LIB_INIT(libSDL3, libSDL3_guest_init)
