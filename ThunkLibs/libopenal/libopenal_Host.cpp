#define AL_DISABLE_NOEXCEPT
#include <AL/al.h>
#include <AL/alc.h>

#include "common/Host.h"

#if defined(__ANDROID__)
#include <stdlib.h>
__attribute__((constructor)) static void vexa_openal_drivers(void) {
  /* AerialX picks the first backend that initializes. Force OpenSL and leave
     the real-time priority at its default of 0. */
  setenv("ALSOFT_DRIVERS", "opensl", 1);
  setenv("ALSOFT_LOGLEVEL", "2", 0);
}
#endif

#include "thunkgen_host_libopenal.inl"

EXPORTS(libopenal)