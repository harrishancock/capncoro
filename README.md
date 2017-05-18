## Coroutines TS integration with Cap'n Proto

[Presentation slides](https://yfmr259n1wqe5aky96fu.antipath.sandcats.io/index.html)

To build, put `capnp` on the PATH and run:

```sh
cd capncoro
cmake -H. -Bbuild -G "Visual Studio 15 2017"
cmake --build build --config debug
```

Right now, the project will only compile with Visual Studio 2017. Clang/LLVM support will
(hopefully) come soon.
