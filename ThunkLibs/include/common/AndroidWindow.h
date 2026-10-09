#pragma once

#if defined(__ANDROID__)
#include <android/native_window.h>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

inline ANativeWindow* VexaAcquireRuntimeWindow(int timeout_ms) {
  using Getter = ANativeWindow* (*)();
  void* vexa = dlopen("libvexa.so", RTLD_NOW | RTLD_NOLOAD);
  if (!vexa) {
    std::fprintf(stderr, "[VEXA][WINDOW] libvexa.so is not loaded\n");
    return nullptr;
  }

  auto* retained = reinterpret_cast<Getter>(dlsym(vexa, "Vexa_GetRuntimeNativeWindowRetained"));
  auto* plain = reinterpret_cast<Getter>(dlsym(vexa, "Vexa_GetRuntimeNativeWindow"));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    ANativeWindow* window = nullptr;
    if (retained) {
      window = retained();
    }
    if (!window && plain) {
      window = plain();
      if (window) {
        ANativeWindow_acquire(window);
      }
    }
    if (window) {
      const int width = ANativeWindow_getWidth(window);
      const int height = ANativeWindow_getHeight(window);
      if (width > 1 && height > 1) {
        std::fprintf(stderr, "[VEXA][WINDOW] native window %p %dx%d\n", window, width, height);
        return window;
      }
      std::fprintf(stderr, "[VEXA][WINDOW] window %p not ready %dx%d\n", window, width, height);
      ANativeWindow_release(window);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::fprintf(stderr, "[VEXA][WINDOW] timed out after %d ms\n", timeout_ms);
  return nullptr;
}
#endif
