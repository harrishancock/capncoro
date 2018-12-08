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

#pragma once

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
// Coroutine TS integration with kj::Promise<T>.

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

template <typename Self, typename T>
class CoroutineBase {
public:
  template <typename U>
  void return_value(U&& value) {
    static_cast<Self*>(this)->adapter->fulfiller.fulfill(T(kj::fwd<U>(value)));
  }
};
template <typename Self>
class CoroutineBase<Self, void> {
public:
  void return_void() {
    static_cast<Self*>(this)->adapter->fulfiller.fulfill();
  }
};
// The Coroutines TS has no `_::FixVoid<T>` equivalent to unify valueful and valueless co_return
// statements, and programs are ill-formed if the "coroutine promise" (Coroutine<T>) has both a
// `return_value()` and `return_void()`. No amount of EnableIffery can get around it, so these
// return_* functions live in a CRTP mixin.

template <typename T>
class Coroutine: public CoroutineBase<Coroutine<T>, T> {
  // The standard calls this the `promise_type` object. I'd prefer to call it the "coroutine
  // implementation object" since the word promise means different things in KJ and std styles. This
  // is where we implement how a `kj::Promise<T>` is returned from a coroutine, and how that promise
  // is later fulfilled. We also fill in a few lifetime-related details.
  //
  // The implementation object is also where we can customize memory allocation of coroutine frames,
  // by implementing a member `operator new(size_t, Args...)` (same `Args...` as in
  // coroutine_traits).
  //
  // We can also customize how await-expressions are transformed within `kj::Promise<T>`-based
  // coroutines by implementing an `await_transform(P)` member function, where `P` is some type for
  // which we want to implement co_await support, e.g. `kj::Promise<U>`. This feature may be useful
  // to provide a more optimized `kj::EventLoop` integration when the coroutine's return type and
  // the await-expression's type are both `kj::Promise` instantiations.

public:
  using Handle = std::experimental::coroutine_handle<Coroutine<T>>;

  ~Coroutine() { if (adapter) adapter->coroutine = nullptr; }
  // The implementation object (*this) lives inside the coroutine's frame, so if it is being
  // destroyed, then that means the frame is being destroyed. Since the frame might be destroyed
  // *before* the adapter (e.g., the coroutine resumes from its last suspension point before the
  // user destroys the owning promise), we need to tell the adapter not to try to double-free the
  // frame.

  Promise<T> get_return_object() {
    return newAdaptedPromise<T, Adapter>(Handle::from_promise(*this));
  }

  auto initial_suspend() { return std::experimental::suspend_never(); }
  auto final_suspend() { return std::experimental::suspend_never(); }
  // These adjust the suspension behavior of coroutines immediately upon initiation, and immediately
  // after completion.
  //
  // The initial suspension point might allow coroutines to always propagate their exceptions
  // asynchronously, even ones thrown before the first co_await.
  //
  // The final suspension point could be useful to delay deallocation of the coroutine frame to
  // match the lifetime of the enclosing promise.

  void unhandled_exception() {
    adapter->fulfiller.rejectIfThrows([] { std::rethrow_exception(std::current_exception()); });
  }

  void set_exception(std::exception_ptr e) {
    // TODO(msvc): Remove when MSVC updates to use unhandled_exception() from the latest TS wording.
    adapter->fulfiller.rejectIfThrows([e = kj::mv(e)] { std::rethrow_exception(kj::mv(e)); });
  }

  template <typename U>
  class Awaiter;

  template <typename U>
  Awaiter<U> await_transform(kj::Promise<U>& promise) { return Awaiter<U>(kj::mv(promise)); }
  template <typename U>
  Awaiter<U> await_transform(kj::Promise<U>&& promise) { return Awaiter<U>(kj::mv(promise)); }
  // T is the return type of the enclosing coroutine, whereas U is the result type of the
  // Promise that the enclosing coroutine is waiting on.

private:
  friend class CoroutineBase<Coroutine<T>, T>;

  struct Adapter;
  Adapter* adapter = nullptr;
  // Set by Adapter's ctor in `get_return_object()`. Nulled out again by Adapter's dtor, if the
  // promise returned from `get_return_object()` is destroyed while this coroutine is suspended.
};

template <typename T>
struct Coroutine<T>::Adapter: Event {
  Adapter(PromiseFulfiller<T>& f, Coroutine::Handle coroutine)
      : fulfiller(f), coroutine(coroutine) {
    coroutine.promise().adapter = this;
  }

  ~Adapter() noexcept(false) {
    // If a coroutine runs to completion, its frame will already have been destroyed by the time the
    // adapter is destroyed, so this if-condition will almost always be false.
    if (coroutine) {
      coroutine.promise().adapter = nullptr;
      coroutine.destroy();
    }
  }

  Maybe<Own<Event>> fire() override {
    // The promise this coroutine is currently waiting on is ready. Extract its result and check for
    // exceptions.

    node->get(*result);

    // Note: the Promise::wait() implementation throws a recoverable exception if both a value and
    // an exception are set. We only have visibility on the exception here, so this might not have
    // quite the same semantics.

    KJ_IF_MAYBE(exception, result->exception) {
      fulfiller.reject(kj::mv(*exception));
      // TODO(now): Ignore exceptions from destroy()?
      coroutine.destroy();
    } else {
      // Call Awaiter::await_resume() and proceed with the coroutine. Note that if this was the last
      // co_await in the coroutine, control will flow off the end of the coroutine and it will be
      // destroyed -- as part of the execution of coroutine.resume().
      coroutine.resume();
    }

    return nullptr;
  }

  PromiseFulfiller<T>& fulfiller;
  Coroutine::Handle coroutine;

  PromiseNode* node = nullptr;
  ExceptionOrValue* result = nullptr;
  // Set by Awaiter::await_suspend().
};

template <typename T>
template <typename U>
class Coroutine<T>::Awaiter {
public:
  explicit Awaiter(Promise<U> promise): promise(kj::mv(promise)) {}

  bool await_ready() const { return false; }
  // TODO(someday): Return "`promise.node.get()` is safe to call".

  U await_resume() {
    KJ_IF_MAYBE(value, result.value) {
      return kj::mv(*value);
    }
    // Since the coroutine promise adapter should have checked for exceptions before resuming the
    // coroutine, 
    KJ_UNREACHABLE;
  }

  void await_suspend(Coroutine::Handle coroutine) {
    auto& node = Promise<FriendHack>::getNode(promise);
    auto adapter = coroutine.promise().adapter;

    // Before suspending, schedule a wakeup call for when the promise we're waiting on is ready.
    // We use the enclosing coroutine's promise adapter as an Event. It's tempting to make this
    // Awaiter be the Event, since this class is where both the promise and space for the result
    // of the promise live, meaning it can most conveniently extract the result. However, whatever
    // Event is used must be able to destroy the coroutine from within the `fire()` callback,
    // which rules out using anything that lives inside the coroutine frame as the Event,
    // including Awaiter and Coroutine<T> itself. The Adapter is the only object that lives
    // outside the coroutine frame.
    node.onReady(adapter);

    // Give the adapter some pointers to our internals so that it can perform the result
    // extraction. It doesn't need to know anything about the type U, but it needs to be able to
    // check for exceptions, in which case the coroutine can be be immediately destroyed (the
    // alternative being a pointless resume only to rethrow the exception).
    adapter->node = &node;
    // Awaiter is movable, but it is safe to store a pointer to `result` in Adapter, because
    // by the time `await_suspend()` is called, Awaiter has already been returned from
    // `await_transform()` and stored in its final resting place in the coroutine's frame.
    adapter->result = &result;
  }

private:
  Promise<U> promise;
  ExceptionOr<FixVoid<U>> result;
};

}  // namespace _ (private)
}  // namespace kj

// =======================================================================================
// Coroutine traits glue
//
// When the compiler sees that a function is implemented as a coroutine (i.e., it contains a co_*
// keyword), it looks up the function's return type in a traits class:
//
//   std::experimentalL::coroutine_traits<kj::Promise<T>, Args...>::promise_type
//       = kj::_::Coroutine<T>

namespace std {
  namespace experimental {
    template <class T, class... Args>
    struct coroutine_traits<kj::Promise<T>, Args...> {
      using promise_type = kj::_::Coroutine<T>;
    };
  }  // namespace experimental
}  // namespace std

#endif  // KJ_HAVE_COROUTINE