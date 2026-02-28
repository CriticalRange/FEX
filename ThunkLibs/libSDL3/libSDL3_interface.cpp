#include <common/GeneratorInterface.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

template<auto>
struct fex_gen_config {
  unsigned version = 1;
};

template<typename>
struct fex_gen_type {};

template<>
struct fex_gen_type<void> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Window> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_GLContextState> : fexgen::opaque_type {};

int FEX_SDL_Init(SDL_InitFlags flags);
void FEX_SDL_Quit();
SDL_Window* FEX_SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags);
void FEX_SDL_DestroyWindow(SDL_Window* window);
int FEX_SDL_GetWindowSize(SDL_Window* window, int* w, int* h);
SDL_GLContext FEX_SDL_GL_CreateContext(SDL_Window* window);
void FEX_SDL_GL_DeleteContext(SDL_GLContext context);
int FEX_SDL_GL_SetSwapInterval(int interval);
int FEX_SDL_GL_SwapWindow(SDL_Window* window);
int FEX_SDL_PollEvent(void* event);
Uint64 FEX_SDL_GetTicks();
void FEX_SDL_Delay(Uint32 ms);
int FEX_SDL_StartTextInput(SDL_Window* window);
int FEX_SDL_StopTextInput(SDL_Window* window);
int FEX_SDL_TextInputActive(SDL_Window* window);

template<>
struct fex_gen_config<FEX_SDL_Init> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_Quit> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_CreateWindow> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_DestroyWindow> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GetWindowSize> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GL_CreateContext> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GL_DeleteContext> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GL_SetSwapInterval> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GL_SwapWindow> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_PollEvent> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GetTicks> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_Delay> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_StartTextInput> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_StopTextInput> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_TextInputActive> : fexgen::custom_host_impl {};
