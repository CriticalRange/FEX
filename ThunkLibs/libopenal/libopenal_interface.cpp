#define AL_DISABLE_NOEXCEPT
#include <common/GeneratorInterface.h>

#include <AL/al.h>
#include <AL/alc.h>

template<auto> struct fex_gen_config {};
template<typename> struct fex_gen_type {};
template<> struct fex_gen_type<ALCdevice> : fexgen::opaque_type {};
template<> struct fex_gen_type<ALCcontext> : fexgen::opaque_type {};

template<> struct fex_gen_config<alcOpenDevice> {};
template<> struct fex_gen_config<alcCloseDevice> {};