#include <common/GeneratorInterface.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// FEX Generation
template<auto> struct fex_gen_config {};
template<typename> struct fex_gen_type {};

// VEXA_FIXES: Expand SDL3 thunk coverage used by current Android bring-up.
// Keep this list explicit so missing imports are easy to spot in reviews.
// SDL Initialization
template<> struct fex_gen_config<SDL_SetMainReady> {};
template<> struct fex_gen_config<SDL_InitSubSystem> {};
template<> struct fex_gen_config<SDL_QuitSubSystem> {};

// SDL Platform
template<> struct fex_gen_config<SDL_GetPlatform> {};

// SDL Windowing
template<> struct fex_gen_type<SDL_Window> :fexgen::opaque_type {};
template<> struct fex_gen_config<SDL_CreateWindow> {};
template<> struct fex_gen_config<SDL_DestroyWindow> {};
template<> struct fex_gen_config<SDL_GetWindowSize> {};
template<> struct fex_gen_config<SDL_GetWindowSizeInPixels> {};
template<> struct fex_gen_config<SDL_GetWindowFlags> {};

// SDL Message boxes
template<> struct fex_gen_config<SDL_ShowMessageBox> {};
template<> struct fex_gen_type<SDL_MessageBoxData> : fexgen::opaque_type {};
template<> struct fex_gen_config<SDL_ShowSimpleMessageBox> {};

// SDL Errors
template<> struct fex_gen_config<SDL_GetError> {};
template<> struct fex_gen_config<SDL_ClearError> {};
