#include <common/GeneratorInterface.h>

#include <SDL3/SDL.h>
template<auto> struct fex_gen_config {};
template<> struct fex_gen_config<SDL_InitSubSystem> {};
template<> struct fex_gen_config<SDL_QuitSubSystem> {};
