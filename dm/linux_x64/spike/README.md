# spike — phase 0: dynarmic alone

The smallest program that proves the mechanism the Linux on x86_64 port relies on ([the plan](../../../docs/plan_linux_x64.md)): x86_64 Linux executing a guest arm64 function through dynarmic, with the guest calling the host through `svc #0` — exactly what the game does with Horizon's SVCs.

The guest program (hand-assembled in `main.cpp`):

1. computes `fib(10)` with a loop;
2. stores it to guest memory;
3. writes `'D','M','H','L'` to guest memory and executes `svc #0`; the **host** side (`CallSVC`) reads those bytes from its own view of guest memory, sums them and writes the sum back at a fixed guest address;
4. the guest loads the sum, adds it to `fib(10)`, stores the total and stops at `brk #0`; the host's `ExceptionRaised` halts the `Jit`.

The host then checks the fib value (55), the checksum (0x125) and the total (0x15C), and prints one `[PASS]`/`[FAIL]` line per check.

## Building and running

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/dm_linux_x64_spike        # no root, no game files, no arm64 toolchain
```

Requirements: cmake 3.16+, a C++20 compiler, ninja (or make) and Boost headers 1.57 or newer (`boost/icl`, `boost/variant` and their dependencies; Debian/Ubuntu: `apt install libboost-dev`). dynarmic itself is fetched by CMake at configure time from a pinned commit (about 4 MB, `URL_HASH` checked) — nothing else is downloaded and nothing is installed system-wide. A custom Boost location: `cmake -S . -B build -DBoost_INCLUDE_DIR=<headers>`. CMake 4.x is fine: the vendored robin-map's old `cmake_minimum_required` is handled in the spike's `CMakeLists.txt`.

The build defaults to the A64 front end only and Release. Everything after this spike (the bridge to the host, the Vulkan front end, the Steam Deck packaging) is described in [the plan](../../../docs/plan_linux_x64.md).
