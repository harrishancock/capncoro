// Copyright (c) 2017 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// Coroutines TS integration with the kj-async event loop.
//
// This header implements the types and functions required to:
//
//   - encapsulate a stackless coroutine in a `kj::Promise<T>`
//   - use a `kj::Promise<T>` in an await-expression

#ifndef KJ_COMPAT_COROUTINE_H_
#define KJ_COMPAT_COROUTINE_H_

#if defined(__GNUC__) && !KJ_HEADER_WARNINGS
#pragma GCC system_header
#endif

#include <kj/async.h>

#ifdef __cpp_coroutines
// Clang uses the official symbol and header file.

#define KJ_HAVE_COROUTINE 1
#include <experimental/coroutine>

#elif defined(_RESUMABLE_FUNCTIONS_SUPPORTED)
// MSVC as of VS2017 still uses their old symbol and header.

#define KJ_HAVE_COROUTINE 1
#include <experimental/resumable>

#endif

#ifdef KJ_HAVE_COROUTINE

// =======================================================================================
// CoroutineImpl
//
// When the compiler sees that a function is implemented as a coroutine (i.e., it contains a co_*
// keyword), it looks up the function's return type in a traits class:
// 
//   std::experimentalL::coroutine_traits<kj::Promise<T>, Args...>::promise_type
//       = kj::_::CoroutineImpl<T>
//
// The standard calls this the `promise_type` object. I'd prefer to call it the "coroutine
// implementation object" since the word promise means different things in KJ and std styles. This
// is where we implement how a `kj::Promise<T>` is returned from a coroutine, and how that promise
// is later fulfilled. We also fill in a few lifetime-related details.
//
// The implementation object is also where we can customize memory allocation of coroutine frames,
// by implementing a member `operator new(size_t, Args...)` (same `Args...` as in coroutine_traits).
//
// We can also customize how await-expressions are transformed within `kj::Promise<T>`-based
// coroutines by implementing an `await_transform(P)` member function, where `P` is some type for
// which we want to implement co_await support, e.g. `kj::Promise<U>`. This feature may be useful to
// provide a more optimized `kj::EventLoop` integration when the coroutine's return type and the
// await-expression's type are both `kj::Promise` instantiations.

namespace kj {
namespace _ {

struct CoroutineAdapter;

template <typename T>
class CoroutineImplBase {
public:
  Promise<T> get_return_object() {
    return newAdaptedPromise<T, CoroutineAdapter>(*this);
  }

  auto initial_suspend() { return std::experimental::suspend_never{}; }
  auto final_suspend() { return std::experimental::suspend_never{}; }
  // These adjust the suspension behavior of coroutines immediately upon initiation, and immediately
  // after completion. The latter would be useful to delay deallocation of the coroutine frame to
  // match the lifetime of the enclosing promise. For KJ-integration purposes, I think we can tell
  // the compiler never to suspend at these two points.

  void unhandled_exception() {
    fulfiller->rejectIfThrows([] { std::rethrow_exception(std::current_exception()); });
  }

  void set_exception(std::exception_ptr e) {
    // TODO(msvc): Remove when MSVC updates to use unhandled_exception() from the latest TS wording.
    fulfiller->rejectIfThrows([e = mv(e)] { std::rethrow_exception(mv(e)); });
  }

  ~CoroutineImplBase();

protected:
  friend struct CoroutineAdapter;
  CoroutineAdapter* adapter;
  PromiseFulfiller<T>* fulfiller;
  // These get filled in by `CoroutineAdapter`'s constructor.
};

template <typename T>
class CoroutineImpl: public CoroutineImplBase<T> {
public:
  void return_value(T&& value) { this->fulfiller->fulfill(mv(value)); }
};
template <>
class CoroutineImpl<void>: public CoroutineImplBase<void> {
public:
  void return_void() { this->fulfiller->fulfill(); }
};
// The Coroutines TS has no `_::FixVoid<T>` equivalent to unify valueful and valueless co_return
// statements, so these return_* functions must live in separate types.

struct CoroutineAdapter {
  template <typename T>
  CoroutineAdapter(PromiseFulfiller<T>& f, CoroutineImplBase<T>& impl)
      : coroutine(std::experimental::coroutine_handle<>::from_address(static_cast<void*>(&impl))) {
    impl.adapter = this;
    impl.fulfiller = &f;
  }

  ~CoroutineAdapter() noexcept(false) { if (coroutine) { coroutine.destroy(); } }
  // If a coroutine runs to completion, its frame will already have been destroyed by the time the
  // adapter is destroyed, so this if-condition will almost always be falsey.

  std::experimental::coroutine_handle<> coroutine;
};

template <typename T>
CoroutineImplBase<T>::~CoroutineImplBase() { adapter->coroutine = nullptr; }
// The implementation object (*this) lives inside the coroutine's frame, so if it is being
// destroyed, then that means the frame is being destroyed. Since the frame might be destroyed
// *before* the adapter (e.g., the coroutine resumes from its last suspension point before the
// user destroys the owning promise), we need to tell the adapter not to try to double-free the
// frame.

}  // namespace _ (private)
}  // namespace kj

namespace std {
  namespace experimental {
    template <class T, class... Args>
    struct coroutine_traits<kj::Promise<T>, Args...> {
      using promise_type = kj::_::CoroutineImpl<T>;
    };
  }  // namespace experimental
}  // namespace std

// =======================================================================================
// co_await kj::Promise implementation
//
// The above `CoroutineImpl` type allows functions returning `kj::Promise<T>` to use coroutine
// syntax. This only buys us `co_return`, though: `co_await` requires further integration with its
// expression type.

namespace kj {
namespace _ {

template <typename T>
class PromiseAwaiter {
public:
  PromiseAwaiter(Promise<T>&& p): promise(mv(p)) {}

  bool await_ready() const { return false; }

  T await_resume() {
    // Copied from Promise::wait() implementation.
    KJ_IF_MAYBE(value, result.value) {
      KJ_IF_MAYBE(exception, result.exception) {
        throwRecoverableException(kj::mv(*exception));
      }
      return _::returnMaybeVoid(kj::mv(*value));
    } else KJ_IF_MAYBE(exception, result.exception) {
      throwFatalException(kj::mv(*exception));
    } else {
      // Result contained neither a value nor an exception?
      KJ_UNREACHABLE;
    }
  }

  void await_suspend(std::experimental::coroutine_handle<> c) {
    promise2 = promise.then(onValue(c), onException(c)).eagerlyEvaluate(nullptr);
  }

private:
  auto onValue(std::experimental::coroutine_handle<> c) {
    return [this, c](T&& r) { return resume(c, mv(r)); };
  }
  auto onException(std::experimental::coroutine_handle<> c) {
    return [this, c](Exception&& e) { return resume(c, {false, mv(e)}); };
  }

  Promise<void> resume(std::experimental::coroutine_handle<> c, ExceptionOr<FixVoid<T>>&& r) {
    result = mv(r);
    KJ_DEFER(c.resume());
    return mv(promise2);
    // We are currently executing in a `promise2.then()` continuation, but `c.resume()` destroys
    // `this`, which owns `promise2`. Since that would lead to undefined behavior, return `promise2`
    // instead.
  }

  Promise<T> promise;
  Promise<void> promise2{NEVER_DONE};
  ExceptionOr<FixVoid<T>> result;
};

template <>
inline auto PromiseAwaiter<void>::onValue(std::experimental::coroutine_handle<> c) {
  // Special case for `Promise<void>`.
  return [this, c]() { return resume(c, Void{}); };
}

}  // namespace _ (private)

template <class T>
auto operator co_await(Promise<T>& promise) { return _::PromiseAwaiter<T>{mv(promise)}; }
template <class T>
auto operator co_await(Promise<T>&& promise) { return _::PromiseAwaiter<T>{mv(promise)}; }
// Asynchronously wait for a promise inside of a coroutine.
//
// Like .then() and friends, operator co_await consumes the promise passed to it, regardless of
// the promise's lvalue-ness. Instead of returning a new promise to you, it stores it inside an
// Awaitable, as defined by the Coroutines TS, which lives in the enclosing coroutine's frame.

}  // namespace kj

#endif  // KJ_HAVE_COROUTINE

#endif  // KJ_COMPAT_COROUTINE_H_