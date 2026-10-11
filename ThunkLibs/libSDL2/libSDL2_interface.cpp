/*
$info$
tags: thunklibs|SDL2
desc: SDL2 guest/host thunk interface for the VEXA bridge entrypoints
$end_info$
*/

#include <common/GeneratorInterface.h>

#include <SDL.h>

template<auto>
struct fex_gen_config {};

template<typename>
struct fex_gen_type {};

// VEXA_FIXES: Treat opaque host-managed handles as passthrough pointer tokens
// across the thunk boundary (no guest-side struct layout assumptions).
template<>
struct fex_gen_type<void> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Window> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Surface> : fexgen::opaque_type {};

template<>
struct fex_gen_type<SDL_Cursor> : fexgen::opaque_type {};

// VEXA_FIXES: Expose bridge entrypoints with FEX_SDL2_ prefix to separate them
// from native SDL2 symbols loaded by the runtime overlay.
int FEX_SDL2_Init(Uint32 flags);
void FEX_SDL2_Quit(void);
SDL_Window* FEX_SDL2_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags);
void FEX_SDL2_DestroyWindow(SDL_Window* window);
void FEX_SDL2_GetWindowSize(SDL_Window* window, int* w, int* h);
SDL_GLContext FEX_SDL2_GL_CreateContext(SDL_Window* window);
int FEX_SDL2_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context);
void FEX_SDL2_GL_DeleteContext(SDL_GLContext context);
int FEX_SDL2_GL_SetSwapInterval(int interval);
void FEX_SDL2_GL_SwapWindow(SDL_Window* window);
int FEX_SDL2_PollEvent(void* event);
Uint32 FEX_SDL2_GetTicks(void);
Uint64 FEX_SDL2_GetTicks64(void);
void FEX_SDL2_Delay(Uint32 ms);
int FEX_SDL2_StartTextInput(void);
void FEX_SDL2_StopTextInput(void);
SDL_bool FEX_SDL2_IsTextInputActive(void);
SDL_Surface* FEX_SDL2_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface* FEX_SDL2_CreateRGBSurfaceFrom(void* pixels, int width, int height, int depth, int pitch, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
void FEX_SDL2_FreeSurface(SDL_Surface* surface);
int FEX_SDL2_UpperBlitScaled(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect);
int FEX_SDL2_SetSurfaceBlendMode(SDL_Surface* surface, SDL_BlendMode blendMode);
SDL_Cursor* FEX_SDL2_CreateColorCursor(SDL_Surface* surface, int hot_x, int hot_y);
SDL_Cursor* FEX_SDL2_CreateSystemCursor(SDL_SystemCursor id);
void FEX_SDL2_SetCursor(SDL_Cursor* cursor);
SDL_Cursor* FEX_SDL2_GetCursor(void);
SDL_Cursor* FEX_SDL2_GetDefaultCursor(void);
void FEX_SDL2_FreeCursor(SDL_Cursor* cursor);
int FEX_SDL2_ShowCursor(int toggle);

// VEXA_FIXES: SDL2 bridge functions are backed by custom host implementations
// rather than direct host symbol forwarding for this function set.
template<>
struct fex_gen_config<FEX_SDL2_Init> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_Quit> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_CreateWindow> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_DestroyWindow> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GetWindowSize> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GL_CreateContext> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GL_MakeCurrent> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GL_DeleteContext> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GL_SetSwapInterval> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GL_SwapWindow> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_PollEvent> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GetTicks> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GetTicks64> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_Delay> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_StartTextInput> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_StopTextInput> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_IsTextInputActive> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_CreateRGBSurface> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_CreateRGBSurfaceFrom> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_FreeSurface> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_UpperBlitScaled> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_SetSurfaceBlendMode> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_CreateColorCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_CreateSystemCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_SetCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GetCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_GetDefaultCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_FreeCursor> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_SDL2_ShowCursor> : fexgen::custom_host_impl {};
