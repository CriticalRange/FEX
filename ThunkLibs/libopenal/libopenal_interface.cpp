#define AL_DISABLE_NOEXCEPT
#define AL_ALEXT_PROTOTYPES
#include <common/GeneratorInterface.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

template<auto> struct fex_gen_config {};
template<typename> struct fex_gen_type {};
template<> struct fex_gen_type<ALCdevice> : fexgen::opaque_type {};
template<> struct fex_gen_type<ALCcontext> : fexgen::opaque_type {};

// ALC core
template<> struct fex_gen_config<alcCreateContext> {};
template<> struct fex_gen_config<alcMakeContextCurrent> {};
template<> struct fex_gen_config<alcProcessContext> {};
template<> struct fex_gen_config<alcSuspendContext> {};
template<> struct fex_gen_config<alcDestroyContext> {};
template<> struct fex_gen_config<alcGetCurrentContext> {};
template<> struct fex_gen_config<alcGetContextsDevice> {};
template<> struct fex_gen_config<alcOpenDevice> {};
template<> struct fex_gen_config<alcCloseDevice> {};
template<> struct fex_gen_config<alcGetError> {};
template<> struct fex_gen_config<alcIsExtensionPresent> {};
template<> struct fex_gen_config<alcGetProcAddress> {};
template<> struct fex_gen_config<alcGetEnumValue> {};
template<> struct fex_gen_config<alcGetString> {};
template<> struct fex_gen_config<alcGetIntegerv> {};
template<> struct fex_gen_config<alcCaptureOpenDevice> {};
template<> struct fex_gen_config<alcCaptureCloseDevice> {};
template<> struct fex_gen_config<alcCaptureStart> {};
template<> struct fex_gen_config<alcCaptureStop> {};
template<> struct fex_gen_config<alcCaptureSamples> {};

// AL core + 1.1
template<> struct fex_gen_config<alEnable> {};
template<> struct fex_gen_config<alDisable> {};
template<> struct fex_gen_config<alIsEnabled> {};
template<> struct fex_gen_config<alGetString> {};
template<> struct fex_gen_config<alGetBooleanv> {};
template<> struct fex_gen_config<alGetIntegerv> {};
template<> struct fex_gen_config<alGetFloatv> {};
template<> struct fex_gen_config<alGetDoublev> {};
template<> struct fex_gen_config<alGetBoolean> {};
template<> struct fex_gen_config<alGetInteger> {};
template<> struct fex_gen_config<alGetFloat> {};
template<> struct fex_gen_config<alGetDouble> {};
template<> struct fex_gen_config<alGetError> {};
template<> struct fex_gen_config<alIsExtensionPresent> {};
template<> struct fex_gen_config<alGetProcAddress> {};
template<> struct fex_gen_config<alGetEnumValue> {};
template<> struct fex_gen_config<alListenerf> {};
template<> struct fex_gen_config<alListener3f> {};
template<> struct fex_gen_config<alListenerfv> {};
template<> struct fex_gen_config<alListeneri> {};
template<> struct fex_gen_config<alListener3i> {};
template<> struct fex_gen_config<alListeneriv> {};
template<> struct fex_gen_config<alGetListenerf> {};
template<> struct fex_gen_config<alGetListener3f> {};
template<> struct fex_gen_config<alGetListenerfv> {};
template<> struct fex_gen_config<alGetListeneri> {};
template<> struct fex_gen_config<alGetListener3i> {};
template<> struct fex_gen_config<alGetListeneriv> {};
template<> struct fex_gen_config<alGenSources> {};
template<> struct fex_gen_config<alDeleteSources> {};
template<> struct fex_gen_config<alIsSource> {};
template<> struct fex_gen_config<alSourcef> {};
template<> struct fex_gen_config<alSource3f> {};
template<> struct fex_gen_config<alSourcefv> {};
template<> struct fex_gen_config<alSourcei> {};
template<> struct fex_gen_config<alSource3i> {};
template<> struct fex_gen_config<alSourceiv> {};
template<> struct fex_gen_config<alGetSourcef> {};
template<> struct fex_gen_config<alGetSource3f> {};
template<> struct fex_gen_config<alGetSourcefv> {};
template<> struct fex_gen_config<alGetSourcei> {};
template<> struct fex_gen_config<alGetSource3i> {};
template<> struct fex_gen_config<alGetSourceiv> {};
template<> struct fex_gen_config<alSourcePlayv> {};
template<> struct fex_gen_config<alSourceStopv> {};
template<> struct fex_gen_config<alSourceRewindv> {};
template<> struct fex_gen_config<alSourcePausev> {};
template<> struct fex_gen_config<alSourcePlay> {};
template<> struct fex_gen_config<alSourceStop> {};
template<> struct fex_gen_config<alSourceRewind> {};
template<> struct fex_gen_config<alSourcePause> {};
template<> struct fex_gen_config<alSourceQueueBuffers> {};
template<> struct fex_gen_config<alSourceUnqueueBuffers> {};
template<> struct fex_gen_config<alGenBuffers> {};
template<> struct fex_gen_config<alDeleteBuffers> {};
template<> struct fex_gen_config<alIsBuffer> {};
template<> struct fex_gen_config<alBufferData> {};
template<> struct fex_gen_config<alBufferf> {};
template<> struct fex_gen_config<alBuffer3f> {};
template<> struct fex_gen_config<alBufferfv> {};
template<> struct fex_gen_config<alBufferi> {};
template<> struct fex_gen_config<alBuffer3i> {};
template<> struct fex_gen_config<alBufferiv> {};
template<> struct fex_gen_config<alGetBufferf> {};
template<> struct fex_gen_config<alGetBuffer3f> {};
template<> struct fex_gen_config<alGetBufferfv> {};
template<> struct fex_gen_config<alGetBufferi> {};
template<> struct fex_gen_config<alGetBuffer3i> {};
template<> struct fex_gen_config<alGetBufferiv> {};
template<> struct fex_gen_config<alDopplerFactor> {};
template<> struct fex_gen_config<alDopplerVelocity> {};
template<> struct fex_gen_config<alSpeedOfSound> {};
template<> struct fex_gen_config<alDistanceModel> {};

// AL EFX
template<> struct fex_gen_config<alGenEffects> {};
template<> struct fex_gen_config<alDeleteEffects> {};
template<> struct fex_gen_config<alIsEffect> {};
template<> struct fex_gen_config<alEffecti> {};
template<> struct fex_gen_config<alEffectiv> {};
template<> struct fex_gen_config<alEffectf> {};
template<> struct fex_gen_config<alEffectfv> {};
template<> struct fex_gen_config<alGetEffecti> {};
template<> struct fex_gen_config<alGetEffectiv> {};
template<> struct fex_gen_config<alGetEffectf> {};
template<> struct fex_gen_config<alGetEffectfv> {};

template<> struct fex_gen_config<alGenFilters> {};
template<> struct fex_gen_config<alDeleteFilters> {};
template<> struct fex_gen_config<alIsFilter> {};
template<> struct fex_gen_config<alFilteri> {};
template<> struct fex_gen_config<alFilteriv> {};
template<> struct fex_gen_config<alFilterf> {};
template<> struct fex_gen_config<alFilterfv> {};
template<> struct fex_gen_config<alGetFilteri> {};
template<> struct fex_gen_config<alGetFilteriv> {};
template<> struct fex_gen_config<alGetFilterf> {};
template<> struct fex_gen_config<alGetFilterfv> {};

template<> struct fex_gen_config<alGenAuxiliaryEffectSlots> {};
template<> struct fex_gen_config<alDeleteAuxiliaryEffectSlots> {};
template<> struct fex_gen_config<alIsAuxiliaryEffectSlot> {};
template<> struct fex_gen_config<alAuxiliaryEffectSloti> {};
template<> struct fex_gen_config<alAuxiliaryEffectSlotiv> {};
template<> struct fex_gen_config<alAuxiliaryEffectSlotf> {};
template<> struct fex_gen_config<alAuxiliaryEffectSlotfv> {};
template<> struct fex_gen_config<alGetAuxiliaryEffectSloti> {};
template<> struct fex_gen_config<alGetAuxiliaryEffectSlotiv> {};
template<> struct fex_gen_config<alGetAuxiliaryEffectSlotf> {};
template<> struct fex_gen_config<alGetAuxiliaryEffectSlotfv> {};
