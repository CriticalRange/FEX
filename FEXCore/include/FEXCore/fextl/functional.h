// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/AllocatorHooks.h>

#include <functional>
#include <type_traits>
#include <utility>

namespace fextl {

/**
 * Equivalent to std::move_only_function but uses FEXCore::Allocator routines
 * for non-function pointers.
 */
template<typename F, void* (*Alloc)(size_t, size_t) = ::FEXCore::Allocator::aligned_alloc, void (*Dealloc)(void*) = ::FEXCore::Allocator::aligned_free>
class move_only_function;

template<typename R, typename... Args, void* (*Alloc)(size_t, size_t), void (*Dealloc)(void*)>
class move_only_function<R(Args...), Alloc, Dealloc> {
public:
  template<typename F>
  requires std::is_invocable_r_v<R, F, Args...>
  move_only_function(F&& f) noexcept(std::is_nothrow_move_constructible_v<F>) {
    if constexpr (std::is_convertible_v<F, R (*)(Args...)>) {
      // Argument is a function pointer, a captureless lambda, or a stateless function object.
      // std::function can store these without allocation
      internal = std::move(f);
    } else if constexpr (std::is_nothrow_constructible_v<std::function<R(Args...)>, F>) {
      // If construction is guaranteed not to throw an exception, this implies
      // the std::function implementation won't allocate memory!
      internal = std::move(f);
    } else {
      // Other arguments require allocation, which is a problem since
      // std::function doesn't allow allocator customization. Implementations
      // are generally able to avoid allocation for lambdas with a single
      // pointer capture however. We can exploit this special case by wrapping
      // the actual argument in a lambda that points an external storage
      // location.

      static_assert(!std::is_pointer_v<F>, "Pointer types must manually be dereferenced");

      // First, relocate argument to a location returned from FEX's allocators
      using Fnoref = std::remove_reference_t<F>;
      storage = Alloc(alignof(Fnoref), sizeof(Fnoref));
      new (storage) Fnoref {std::move(f)};

      // Second, keep a direct invocation trampoline instead of routing through
      // std::function assignment. Android libc++ may reject the noexcept path.
      // TODO: Replace this compatibility path with std::move_only_function (or
      // equivalent) once the NDK exposes a suitable implementation.
      internal_invoke = [](const move_only_function* self, Args&&... args) -> R {
        auto* fn = reinterpret_cast<Fnoref*>(self->storage);
        if constexpr (std::is_void_v<R>) {
          (*fn)(std::forward<Args>(args)...);
          return;
        } else {
          return (*fn)(std::forward<Args>(args)...);
        }
      };

      // Finally, if a destructor must be called, generate a pointer to it.
      if constexpr (!std::is_trivially_destructible_v<Fnoref>) {
        internal_destructor = [](move_only_function* self) {
          reinterpret_cast<Fnoref*>(self->storage)->~Fnoref();
        };
      }
    }
  }

  move_only_function() noexcept {}
  move_only_function(std::nullptr_t) noexcept {}
  move_only_function(const move_only_function&) = delete;
  move_only_function(move_only_function&& other) noexcept {
    *this = std::move(other);
  }

  move_only_function& operator=(move_only_function&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    reset();
    internal = std::exchange(other.internal, nullptr);
    internal_invoke = std::exchange(other.internal_invoke, nullptr);
    internal_destructor = std::exchange(other.internal_destructor, nullptr);
    storage = std::exchange(other.storage, nullptr);
    return *this;
  }

  ~move_only_function() {
    reset();
  }

  R operator()(Args... args) const {
    if (internal_invoke) {
      if constexpr (std::is_void_v<R>) {
        internal_invoke(this, std::forward<Args>(args)...);
        return;
      } else {
        return internal_invoke(this, std::forward<Args>(args)...);
      }
    }
    return internal(std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept {
    return internal_invoke != nullptr || static_cast<bool>(internal);
  }

private:
  void reset() noexcept {
    if (internal_destructor) {
      internal_destructor(this);
      internal_destructor = nullptr;
    }
    if (storage) {
      Dealloc(storage);
      storage = nullptr;
    }
    internal_invoke = nullptr;
    internal = nullptr;
  }

  std::function<R(Args...)> internal;
  R (*internal_invoke)(const move_only_function*, Args&&...) = nullptr;
  void (*internal_destructor)(move_only_function*) = nullptr;
  void* storage = nullptr;
};
} // namespace fextl
