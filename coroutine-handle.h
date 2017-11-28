// -*- C++ -*-
//===----------------------------- coroutine -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP_EXPERIMENTAL_COROUTINE
#define _LIBCPP_EXPERIMENTAL_COROUTINE

/**
    experimental/coroutine synopsis

// C++next

namespace std {
namespace experimental {
inline namespace coroutines_v1 {

  // 18.11.1 coroutine traits
template <typename R, typename... ArgTypes>
class coroutine_traits;
// 18.11.2 coroutine handle
template <typename Promise = void>
class coroutine_handle;
// 18.11.2.7 comparison operators:
bool operator==(coroutine_handle<> x, coroutine_handle<> y) noexcept;
bool operator!=(coroutine_handle<> x, coroutine_handle<> y) noexcept;
bool operator<(coroutine_handle<> x, coroutine_handle<> y) noexcept;
bool operator<=(coroutine_handle<> x, coroutine_handle<> y) noexcept;
bool operator>=(coroutine_handle<> x, coroutine_handle<> y) noexcept;
bool operator>(coroutine_handle<> x, coroutine_handle<> y) noexcept;
// 18.11.3 trivial awaitables
struct suspend_never;
struct suspend_always;
// 18.11.2.8 hash support:
template <class T> struct hash;
template <class P> struct hash<coroutine_handle<P>>;

} // namespace coroutines_v1
} // namespace experimental
} // namespace std

 */

#include <new>
#include <type_traits>
#include <functional>
#include <memory> // for hash<T*>
#include <cstddef>
#include <cassert>


namespace std::experimental {

template <class Tp, class = void>
struct coroutine_traits_sfinae {};

template <class Tp>
struct coroutine_traits_sfinae<
    Tp, std::void_t<typename Tp::promise_type>>
{
  using promise_type = typename Tp::promise_type;
};

template <typename _Ret, typename... _Args>
struct coroutine_traits
    : public coroutine_traits_sfinae<_Ret>
{
};

template <typename Promise = void>
class coroutine_handle;

template <>
class coroutine_handle<void> {
public:
    [[gnu::always_inline, gnu::visibility("hidden")]]
    constexpr coroutine_handle() noexcept : handle(nullptr) {}

    [[gnu::always_inline, gnu::visibility("hidden")]]
    constexpr coroutine_handle(nullptr_t) noexcept : handle(nullptr) {}

    [[gnu::always_inline, gnu::visibility("hidden")]]
    coroutine_handle& operator=(nullptr_t) noexcept {
        handle = nullptr;
        return *this;
    }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    constexpr void* address() const noexcept { return handle; }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    constexpr explicit operator bool() const noexcept { return handle; }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    void operator()() { resume(); }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    void resume() {
      assert(is_suspended() &&
                     "resume() can only be called on suspended coroutines");
      assert(!done() &&
                "resume() has undefined behavior when the coroutine is done");
      __builtin_coro_resume(handle);
    }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    void destroy() {
      assert(is_suspended() &&
                     "destroy() can only be called on suspended coroutines");
      __builtin_coro_destroy(handle);
    }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    bool done() const {
      assert(is_suspended() &&
                     "done() can only be called on suspended coroutines");
      return __builtin_coro_done(handle);
    }

public:
    [[gnu::always_inline, gnu::visibility("hidden")]]
    static coroutine_handle from_address(void* addr) noexcept {
        coroutine_handle tmp;
        tmp.handle = addr;
        return tmp;
    }

    // FIXME: Should from_address(nullptr) be allowed?
    [[gnu::always_inline, gnu::visibility("hidden")]]
    static coroutine_handle from_address(nullptr_t) noexcept {
      return coroutine_handle(nullptr);
    }

    template <class Tp, bool CallIsValid = false>
    static coroutine_handle from_address(Tp*) {
      static_assert(CallIsValid,
       "coroutine_handle<void>::from_address cannot be called with "
        "non-void pointers");
    }

private:
  bool is_suspended() const noexcept  {
    // FIXME actually implement a check for if the coro is suspended.
    return handle;
  }

  template <class PromiseT> friend class coroutine_handle;
  void* handle;
};

// 18.11.2.7 comparison operators:
[[gnu::always_inline, gnu::visibility("hidden")]] inline
bool operator==(coroutine_handle<> x, coroutine_handle<> y) noexcept {
    return x.address() == y.address();
}
[[gnu::always_inline, gnu::visibility("hidden")]] inline
bool operator!=(coroutine_handle<> x, coroutine_handle<> y) noexcept {
    return !(x == y);
}
[[gnu::always_inline, gnu::visibility("hidden")]] inline
bool operator<(coroutine_handle<> x, coroutine_handle<> y) noexcept {
    return std::less<void*>()(x.address(), y.address());
}
[[gnu::always_inline, gnu::visibility("hidden")]] inline
bool operator>(coroutine_handle<> x, coroutine_handle<> y) noexcept {
    return y < x;
}
[[gnu::always_inline, gnu::visibility("hidden")]] inline
bool operator<=(coroutine_handle<> x, coroutine_handle<> y) noexcept {
    return !(x > y);
}
[[gnu::always_inline, gnu::visibility("hidden")]] inline
bool operator>=(coroutine_handle<> x, coroutine_handle<> y) noexcept {
    return !(x < y);
}

template <typename Promise>
class coroutine_handle : public coroutine_handle<> {
    using Base = coroutine_handle<>;
public:
    // 18.11.2.1 construct/reset
    using coroutine_handle<>::coroutine_handle;

    [[gnu::always_inline, gnu::visibility("hidden")]]
    coroutine_handle& operator=(nullptr_t) noexcept {
        Base::operator=(nullptr);
        return *this;
    }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    Promise& promise() const {
        return *reinterpret_cast<Promise*>(
            __builtin_coro_promise(this->handle, alignof(Promise), false));
    }

public:
    [[gnu::always_inline, gnu::visibility("hidden")]]
    static coroutine_handle from_address(void* addr) noexcept {
        coroutine_handle tmp;
        tmp.handle = addr;
        return tmp;
    }

    // NOTE: this overload isn't required by the standard but is needed so
    // the deleted Promise* overload doesn't make from_address(nullptr)
    // ambiguous.
    // FIXME: should from_address work with nullptr?
    [[gnu::always_inline, gnu::visibility("hidden")]]
    static coroutine_handle from_address(nullptr_t) noexcept {
      return coroutine_handle(nullptr);
    }

    template <class Tp, bool CallIsValid = false>
    static coroutine_handle from_address(Tp*) {
      static_assert(CallIsValid,
       "coroutine_handle<promise_type>::from_address cannot be called with "
        "non-void pointers");
    }

    template <bool CallIsValid = false>
    static coroutine_handle from_address(Promise*) {
      static_assert(CallIsValid,
       "coroutine_handle<promise_type>::from_address cannot be used with "
        "pointers to the coroutine's promise type; use 'from_promise' instead");
    }

    [[gnu::always_inline, gnu::visibility("hidden")]]
    static coroutine_handle from_promise(Promise& promise) noexcept {
        typedef typename std::remove_cv<Promise>::type RawPromise;
        coroutine_handle tmp;
        tmp.handle = __builtin_coro_promise(
            std::addressof(const_cast<RawPromise&>(promise)),
             alignof(Promise), true);
        return tmp;
    }
};

struct suspend_never {
  [[gnu::always_inline, gnu::visibility("hidden")]]
  bool await_ready() const noexcept { return true; }
  [[gnu::always_inline, gnu::visibility("hidden")]]
  void await_suspend(coroutine_handle<>) const noexcept {}
  [[gnu::always_inline, gnu::visibility("hidden")]]
  void await_resume() const noexcept {}
};

struct suspend_always {
  [[gnu::always_inline, gnu::visibility("hidden")]]
  bool await_ready() const noexcept { return false; }
  [[gnu::always_inline, gnu::visibility("hidden")]]
  void await_suspend(coroutine_handle<>) const noexcept {}
  [[gnu::always_inline, gnu::visibility("hidden")]]
  void await_resume() const noexcept {}
};

}  // namespace std::experimental

namespace std {

template <class Tp>
struct hash<experimental::coroutine_handle<Tp> > {
    using arg_type = experimental::coroutine_handle<Tp>;
    [[gnu::always_inline, gnu::visibility("hidden")]]
    size_t operator()(arg_type const& v) const noexcept
    {return hash<void*>()(v.address());}
};

}  // namespace std

#endif /* _LIBCPP_EXPERIMENTAL_COROUTINE */