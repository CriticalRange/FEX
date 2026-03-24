#include <common/GeneratorInterface.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

template<auto>
struct fex_gen_config {
  unsigned version = 1;
};

template<typename>
struct fex_gen_type {};

// VEXA_FIXES: Treat opaque host-managed handles as passthrough pointer tokens
// across the thunk boundary (no guest-side struct layout assumptions).
template<>
struct fex_gen_type<void> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Window> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_GLContextState> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Surface> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Cursor> : fexgen::opaque_type {};

// VEXA_FIXES: Expose Android-specific bridge entrypoints with FEX_ prefix to
// separate them from native SDL3 symbols loaded by the runtime overlay.
int FEX_SDL_Init(SDL_InitFlags flags);
void FEX_SDL_Quit();
SDL_Window* FEX_SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags);
void FEX_SDL_DestroyWindow(SDL_Window* window);
int FEX_SDL_GetWindowSize(SDL_Window* window, int* w, int* h);
SDL_GLContext FEX_SDL_GL_CreateContext(SDL_Window* window);
int FEX_SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context);
void FEX_SDL_GL_DeleteContext(SDL_GLContext context);
int FEX_SDL_GL_SetSwapInterval(int interval);
int FEX_SDL_GL_SwapWindow(SDL_Window* window);
int FEX_SDL_PollEvent(void* event);
Uint64 FEX_SDL_GetTicks();
void FEX_SDL_Delay(Uint32 ms);
int FEX_SDL_StartTextInput(SDL_Window* window);
int FEX_SDL_StopTextInput(SDL_Window* window);
int FEX_SDL_TextInputActive(SDL_Window* window);
SDL_Surface* FEX_SDL_CreateSurface(int width, int height, SDL_PixelFormat format);
SDL_Surface* FEX_SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void* pixels, int pitch);
void FEX_SDL_DestroySurface(SDL_Surface* surface);
SDL_Surface* FEX_SDL_ScaleSurface(SDL_Surface* surface, int width, int height, SDL_ScaleMode scaleMode);
SDL_Cursor* FEX_SDL_CreateColorCursor(SDL_Surface* surface, int hot_x, int hot_y);
SDL_Cursor* FEX_SDL_CreateSystemCursor(SDL_SystemCursor id);
bool FEX_SDL_SetCursor(SDL_Cursor* cursor);
SDL_Cursor* FEX_SDL_GetCursor();
SDL_Cursor* FEX_SDL_GetDefaultCursor();
void FEX_SDL_DestroyCursor(SDL_Cursor* cursor);
bool FEX_SDL_ShowCursor();
bool FEX_SDL_HideCursor();
bool FEX_SDL_CursorVisible();

// VEXA_FIXES: SDL3 on Android is backed by custom host implementations rather
// than direct host symbol forwarding for this function set.
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
struct fex_gen_config<FEX_SDL_GL_MakeCurrent> : fexgen::custom_host_impl {};
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
template<>
struct fex_gen_config<FEX_SDL_CreateSurface> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_CreateSurfaceFrom> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_DestroySurface> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_ScaleSurface> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_CreateColorCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_CreateSystemCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_SetCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GetCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_GetDefaultCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_DestroyCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_ShowCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_HideCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL_CursorVisible> : fexgen::custom_host_impl {};
