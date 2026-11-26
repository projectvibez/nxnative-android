# dm/linux_x64 — the host on Linux on x86_64 (Steam Deck)

A port of the nxnative host to x86_64 Linux, for desktop PCs and the **Steam Deck**: the game's arm64 code cannot run natively there, so it is translated while it runs by [dynarmic](https://github.com/lioncash/dynarmic), the way the [Windows on x64 plan](../../docs/plan_windows_x64.md) describes. The full port's plan, what carries over from the Android/macOS hosts and what is new, is [docs/plan_linux_x64.md](../../docs/plan_linux_x64.md).

| Part | State |
|---|---|
| [spike](spike) | Built and runs: dynarmic (A64 front end, x64 back end) at a pinned commit executes a guest arm64 function — a loop, a store to guest memory, an `svc #0` the host answers by writing the sum of the bytes the guest wrote — and a `brk` that halts it; the host checks the results. This is the mechanism every later phase builds on: the game calls the host through SVCs and imports |
| Host, front end, packing | Not started — see the plan's phases |

Building the spike: a C++20 compiler, cmake, ninja and Boost headers (Debian: `libboost-dev`; or `-DBoost_INCLUDE_DIR=<headers>`). No arm64 toolchain. See [spike/README.md](spike/README.md).
