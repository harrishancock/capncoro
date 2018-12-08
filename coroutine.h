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
#if 0
#include <experimental/coroutine>
#else
#include "./coroutine-handle.h"
#endif

#elif defined(_RESUMABLE_FUNCTIONS_SUPPORTED)
// MSVC as of VS2017 still uses their old symbol and header.

#define KJ_HAVE_COROUTINE 1
#include <experimental/resumable>

#endif

#ifdef KJ_HAVE_COROUTINE

// =======================================================================================
// Coroutine
//
// When the compiler sees that a function is implemented as a coroutine (i.e., it contains a co_*
// keyword), it looks up the function's return type in a traits class:
//
//   std::experimentalL::coroutine_traits<kj::Promise<T>, Args...>::promise_type
//       = kj::_::Coroutine<T>
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
class CoroutineBase {
public:
  Promise<T> get_return_object();

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

  ~CoroutineBase();

  Event* getResumeEvent();
  // TODO(now): What happens when an adapter is destroyed before the coroutine is finished? Could
  //   the destructing frame cause another promise to be destroyed, in turn arming the adapter?
  //   Test.

protected:
  friend struct CoroutineAdapter;
  CoroutineAdapter* adapter;
  PromiseFulfiller<T>* fulfiller;
  // These get filled in by `CoroutineAdapter`'s constructor.
};

template <typename T>
class Coroutine: public CoroutineBase<T> {
public:
  void return_value(T&& value) { this->fulfiller->fulfill(mv(value)); }
};
template <>
class Coroutine<void>: public CoroutineBase<void> {
public:
  void return_void() { this->fulfiller->fulfill(); }
};
// The Coroutines TS has no `_::FixVoid<T>` equivalent to unify valueful and valueless co_return
// statements, so these return_* functions must live in separate types.

struct CoroutineAdapter: Event {
  template <typename T>
  CoroutineAdapter(PromiseFulfiller<T>& f,
                   std::experimental::coroutine_handle<Coroutine<T>> c)
      : coroutine(c) {
    c.promise().adapter = this;
    c.promise().fulfiller = &f;
  }

  ~CoroutineAdapter() noexcept(false) { if (coroutine) { coroutine.destroy(); } }
  // If a coroutine runs to completion, its frame will already have been destroyed by the time the
  // adapter is destroyed, so this if-condition will almost always be false.

  Maybe<Own<Event>> fire() override {
    // CoroutineAdapter is an Event rather than Coroutine because Coroutine can be destroyed
    // in the body of `coroutine.resume()`, which would call ~Event(), which would throw because
    // it's being fired.
    coroutine.resume();
    return nullptr;
  }

  std::experimental::coroutine_handle<> coroutine;
};

template <typename T>
Promise<T> CoroutineBase<T>::get_return_object() {
  using Handle = std::experimental::coroutine_handle<Coroutine<T>>;
  auto coroutine = Handle::from_promise(*static_cast<Coroutine<T>*>(this));
  return newAdaptedPromise<T, CoroutineAdapter>(coroutine);
}

template <typename T>
Event* CoroutineBase<T>::getResumeEvent() { return adapter; }

template <typename T>
CoroutineBase<T>::~CoroutineBase() { adapter->coroutine = nullptr; }
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
      using promise_type = kj::_::Coroutine<T>;
    };
  }  // namespace experimental
}  // namespace std

// =======================================================================================
// co_await kj::Promise implementation
//
// The above `Coroutine` type allows functions returning `kj::Promise<T>` to use coroutine
// syntax. This only buys us `co_return`, though: `co_await` requires further integration with its
// expression type.

namespace kj {

// We need a way to access a Promise<T>'s PromiseNode pointer. That requires friend access.
// Fortunately, Promise<T> and its base class give away friendship to all specializations of
// Promise<T>! :D
//
// TODO(cleanup): Either formalize a public promise node API or gain real friendship, e.g. with
//   tighter integration into kj/async.h.
namespace _ { struct FriendHack; }
template <>
struct Promise<_::FriendHack> {
  template <typename T>
  static _::PromiseNode& getNode(Promise<T>& p) { return *p.node; }
};

namespace _ {

template <typename T>
class PromiseAwaiter {
public:
  PromiseAwaiter(Promise<T>&& p): promise(mv(p)) {}

  bool await_ready() const { return false; }
  // TODO(someday): Return "`promise.node.get()` is safe to call".

  T await_resume() {
    ExceptionOr<FixVoid<T>> result;
    getNode().get(result);

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

  template <typename U>
  void await_suspend(std::experimental::coroutine_handle<Coroutine<U>> c) {
    // U is the return type of the currently active coroutine, whereas T is the return type of the
    // Promise we're waiting on (which may or may not be implemented using a coroutine).
    getNode().onReady(c.promise().getResumeEvent());
  }

private:
  Promise<T> promise;
  PromiseNode& getNode() { return Promise<_::FriendHack>::getNode(promise); }
};

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