# Resource Envelope consumer proof

A standalone CMake project that consumes the installed Resource Envelope package through
`find_package(ResourceEnvelope CONFIG REQUIRED)`. It references no build tree, no source
directory and no header path of the library.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<install-prefix>
cmake --build build
./build/consumer <store-directory>
```

The binary exits non-zero if any step fails, so a successful exit is the proof.
