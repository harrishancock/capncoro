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

#include "./coroutine.h"

#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <map>

namespace kj {
namespace {

#ifdef KJ_HAVE_COROUTINE

Promise<int> simpleCoroutine(bool dontThrow = true) {
  KJ_ASSERT(dontThrow);
  co_await Promise<void>(READY_NOW);
  co_return 123;
}

KJ_TEST("Simple coroutine test") {
  EventLoop loop;
  WaitScope waitScope(loop);

  KJ_EXPECT(simpleCoroutine().wait(waitScope) == 123);
}

KJ_TEST("Exceptions get propagated in coroutines") {
  EventLoop loop;
  WaitScope waitScope(loop);

  KJ_EXPECT_THROW_RECOVERABLE(FAILED, simpleCoroutine(false).wait(waitScope));
}

Promise<void> sendData(Promise<Own<NetworkAddress>> addressPromise) {
  auto address = co_await addressPromise;
  auto client = co_await address->connect();
  co_await client->write("foo", 3);
}

Promise<String> receiveDataChained(Promise<Own<NetworkAddress>> addressPromise) {
  auto buffer = heapArray<char>(64);
  auto buf = ArrayPtr<char>(buffer);
  return addressPromise.then([](auto addr) {
    auto listener = addr->listen();
    return listener->accept().attach(mv(listener));
  }).then([buf](auto stream) mutable {
    return stream->read(buf.begin(), 1, buf.size()).attach(mv(stream));
  }).then([buf](size_t n) {
    KJ_EXPECT(3u == n);
    return heapString(buf.begin(), n);
  }).attach(mv(buffer));
}

Promise<String> receiveDataCoroutine(Promise<Own<NetworkAddress>> addressPromise) {
  auto address = co_await addressPromise;
  auto listener = address->listen();
  auto server = co_await listener->accept();
  char buffer[4];
  auto n = co_await server->read(buffer, 3, 4);
  KJ_EXPECT(3u == n);
  co_return heapString(buffer, n);
}

KJ_TEST("Simple network test with chained promise implementation") {
  auto io = setupAsyncIo();
  auto& network = io.provider->getNetwork();

  constexpr uint port = 5500;

  sendData(network.parseAddress("localhost", port)).detach([](Exception&& exception) {
    KJ_FAIL_EXPECT(exception);
  });

  String result = receiveDataChained(network.parseAddress("*", port)).wait(io.waitScope);

  KJ_EXPECT("foo" == result);
}


KJ_TEST("Simple network test with coroutine implementation") {
  auto io = setupAsyncIo();
  auto& network = io.provider->getNetwork();

  constexpr uint port = 5500;

  sendData(network.parseAddress("localhost", port)).detach([](Exception&& exception) {
    KJ_FAIL_EXPECT(exception);
  });

  String result = receiveDataCoroutine(network.parseAddress("*", port)).wait(io.waitScope);

  KJ_EXPECT("foo" == result);
}



Promise<Own<AsyncIoStream>> httpClientConnect(AsyncIoContext& io) {
  auto addr = co_await io.provider->getNetwork().parseAddress("capnproto.org", 80);
  co_return co_await addr->connect();
}

Promise<void> httpClient(AsyncIoContext& io) {
  Own<AsyncIoStream> connection;
  try {
    connection = co_await httpClientConnect(io);
  } catch (const Exception&) {
    KJ_LOG(WARNING, "skipping test because couldn't connect to capnproto.org");
    co_return;
  }
  // Successfully connected to capnproto.org. Try doing GET /. We expect to get a redirect to
  // HTTPS, because what kind of horrible web site would serve in plaintext, really?

  HttpHeaderTable table;
  auto client = newHttpClient(table, *connection);

  HttpHeaders headers(table);
  headers.set(HttpHeaderId::HOST, "capnproto.org");

  auto response = co_await client->request(HttpMethod::GET, "/", headers).response;
  KJ_EXPECT(response.statusCode / 100 == 3);
  auto location = KJ_ASSERT_NONNULL(response.headers->get(HttpHeaderId::LOCATION));
  KJ_EXPECT(location == "https://capnproto.org/");

  auto body = co_await response.body->readAllText();
  co_return;
  // If I comment out the previous co_return, I don't need this one. If I comment out
  // this co_return, return_void() is never called and the program hangs.
}

KJ_TEST("HttpClient to capnproto.org with a coroutine") {
  auto io = setupAsyncIo();

  httpClient(io).wait(io.waitScope);
}

#endif  // KJ_HAVE_COROUTINE

}  // namespace
}  // namespace kj
