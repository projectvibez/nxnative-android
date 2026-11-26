# Plan: the host on Linux on x86_64 (Steam Deck)

Status: a plan; the phase 0 spike is built and runs ([dm/linux_x64/spike](../dm/linux_x64/spike)), nothing else is. A separate build from [the host on Windows on x64](plan_windows_x64.md) and from [the host on Windows on Arm](plan_windows_arm.md): its own host executable compiled for x86_64 Linux (clang or gcc, `x86_64-pc-linux-gnu`), its own directory (`dm/linux_x64/`), CMake project and dependencies (dynarmic, no arm64 toolchain), and its own output from the packer. Source that does not depend on the processor (the window, the Vulkan replay, the audio, the movies and the input) is written once and compiled into the Windows and Linux builds, as `gfx_vk.c` is shared today by Android and nothing else; neither build waits for the other.

## Why it is a different kind of port

Everywhere else this repository runs the game's own arm64 code on an arm64 processor: the rewriter changes only what the operating system forbids, and the
game calls the host directly (its imports bound to host functions, `svc` turned into a branch to `dm_gate`, NVN calls into `nvn_thunks.S`), both sides using
the same calling convention.
An x86_64 processor cannot execute arm64 instructions, and Linux on x86_64 does not emulate arm64 either. The game's code
must be translated to x86_64 while it runs, by a recompiler.

The recompiler is [dynarmic](https://github.com/lioncash/dynarmic) (0BSD licence): an A64 front end and an x64 back end, with the exclusive monitor and
the page-table memory model the Switch emulators built on it used. It is a library: the host gives it guest memory, receives callbacks for `svc`, unknown
instructions and exceptions, and runs one translation context per guest thread.

The target is the **Steam Deck**: a Zen 2 APU (4 cores, 8 threads), RDNA 2 graphics through RADV (Mesa), 16 GB of unified memory, a 1280×800 screen,
SteamOS (Arch-based; `/usr` is read-only and updates atomically), Game Mode on gamescope (Wayland) and Desktop Mode (KDE Plasma). Its built-in controls
present to Steam Input as a standard controller. Nothing of the game's code can run natively there: the Deck is x86_64 and the games are arm64. This is the
same shape of port as the Windows on x64 one, on a platform with far better driver support (RADV implements the whole baseline the NVN replay needs, with BC
compression and large descriptor pools) and with Linux's memory mapping primitives, which remove the Windows plan's largest risk.

Two shortcuts are not this port: the future Windows on x64 build under Proton (possible once it exists, with Wine's overhead and without Game Mode
integration), and Waydroid (an Android runtime inside a container; SteamOS ships none, and it is not a supported configuration there).

## What carries over

| Part | On Linux on x86_64 |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`) | A third target, `DM_TARGET=x64` (the same target the Windows on x64 plan uses): the guest's x18 is a dynarmic guest register, so no operating system needs to reserve it; the behaviour-changing cuts (`patches.py`, `sdk_patches.py`) stay, as guest code |
| Host (`dm/host`) | Builds as on Android: Darwin interfaces from a compat layer (Android's `compat/` headers and `plat_android.c` are the model; most of it is plain C and shared). `crash.c` keeps its `sigaction` with `SA_SIGINFO` handlers — Linux is closer to Darwin here than Windows is |
| NVN replay (`gfx_vk.c`, `gfx_vk_draw.inc`) | Vulkan, with `VK_KHR_wayland_surface` / `VK_KHR_xcb_surface`. RADV on the Deck supports BC texture compression, descriptor indexing, `VK_EXT_transform_feedback`, tessellation and geometry shaders, extended dynamic state and Vulkan 1.3 — the compatibility stand-ins the Adreno 619 needs (CPU-decoded BC, no transform feedback) go unused |
| Shaders | The SPIR-V `tools/spv_build.py` makes, unchanged at first, minus the Adreno rewrites in `spv_android.py`; the packer chooses the shader variant per GPU vendor (AMD RADV, Adreno, desktop NVIDIA/AMD/Intel), as the Windows plan describes |
| Movies | `hle_movie_avc` decoded with libavcodec/libavformat/libavutil to NV12 or RGBA — the `media_android.c` shape with MediaCodec replaced by ffmpeg (SteamOS ships ffmpeg; a Flatpak gets it from `org.freedesktop.Platform`) |
| Audio | SDL2's audio API as the stand-in for AudioToolbox's output AudioUnit, as `coreaudio_aaudio.c` stands in for AAudio (SDL2 talks to PipeWire; the Deck runs PipeWire) |
| Input | SDL2's game controller API: the Deck's controls appear through Steam Input as a standard controller; rumble through SDL2's rumble (the Deck has motors); trackpads and gyro through SDL2's sensor API; there is no touchscreen, so the touch layer stays hidden (physical-controller mode). Text input through SDL2's text input events |
| Front end | A small SDL2 launcher in C (the `dm/mac/launcher.c` model): the game library drawn from each game's `icon.jpg` / `control.nacp` and `launcher.json`, fully gamepad-navigable — Game Mode has no mouse. Keyboard and mouse are optional |
| Packing | `scripts/build_android_from_nsp.py` already runs on Linux; the Linux target writes a folder (host executable, `games/<title ID>/`) or an AppImage/Flatpak instead of an APK, with the same extraction → rewrite → shaders → build stages and the same per-game host library |
| Settings | `dm/launcher.json` per game, `DM_LAUNCHER=0` to skip the launcher, the saved-choices store as on the Mac (a front end writes the preferences directly) |

## What is new

1. **The game's code runs in dynarmic.** Each guest thread (`hle_thread.c`) owns a dynarmic `A64::Jit` whose registers are the thread's; the host's thread
   still owns the host stack and the scheduling. Guest memory is the host's own mapping at the same addresses (reserved with `mmap` marked
   `MAP_FIXED_NOREPLACE`), given to dynarmic as a flat page table with fastmem, so loads and stores are single x86_64 instructions. Aliasing memory (the
   loader's code pages, the NVN physical pools in `hle_nvn.c`) is a `memfd_create` mapped again at the other address — this is the Windows plan's largest risk,
   and Linux does it natively, with no placeholder splitting.
2. **The rewriter does less.** No operating system forbids `x18` or `tpidrro_el0` inside the recompiler: dynarmic presents them as guest registers. The cuts
   that change a game's behaviour (`patches.py`, `sdk_patches.py`: frame steps, observation gates) still apply, as guest code. `DM_TARGET=x64` keeps those
   and drops the register rewrites.
3. **Every call between the game and the host goes through a bridge.** An imported function bound to a host function resolves to an address that ends the
   dynarmic block and enters the host: a table of host functions, each with a generated adapter that reads the arguments from the guest's `x0`–`x7`,
   `v0`–`v7` and stack (AAPCS64) and calls the x86_64 function, then writes the result back. `gen_protos.py` already generates `nvn_thunks.S` from the NVN
   prototypes: it generates these adapters instead, and the same for the service and SDK functions the host implements. Calls the other way (thread entries,
   module initialisers, the game's callbacks such as allocators and NVN's debug callback) run the guest function in the calling thread's `A64::Jit` until it
   returns to a sentinel address. The adapter generator is written once and shared with the Windows on x64 build.
4. **Suspend and resume.** The Deck sleeps instantly (power button, or after a timeout). The host handles the pause as it does on Android: drain the pending
   submissions, stop the audio output, save the pipeline cache and flush the saves; on resume, drop and recreate the swapchain before presenting again. A game
   frozen mid-submit must not resume into a GPU that never reported back.

## Steam Deck specifics

- **SteamOS is Arch-based with a read-only `/usr` and atomic updates.** Extra packages install as Flatpaks; the supported install is a Flatpak
  (`org.nxnative.Deck`, say) or a rootless AppImage. A JIT host needs an executable `memfd` (no W^X sealing); Flatpak allows this for apps that declare it
  (browsers do), and the finish-args go in the app's manifest when the time comes. Desktop Mode users can also run the Windows-style folder build from anywhere,
  with no root and no Flatpak.
- **Game Mode is the storefront.** The launcher is added as a Non-Steam game with its artwork — capsule, grid and hero images generated from the game's
  `control.nacp` icon and box art, like the Android launcher draws its screens. Steam Input passes the Deck's controls (and gyro, and the trackpads as mice) to
  the host as a controller.
- **Power and frame pacing.** The Deck runs about 3–15 W and 1280×800 at 40/60 Hz; `DM_FPS_TARGET=30` (and 40 where the game's pacing allows) is the right
  default, and the frame-rate counter the Android build draws (`DM_SHOW_FPS`) comes along. The Tap/2D/3D touch overlays stay hidden — the Deck has no
  touchscreen.
- **Driver.** RADV only. AMDVLK exists on desktop distros, not on SteamOS; every Vulkan assumption (transform feedback, descriptor indexing, BC, extended
  dynamic state) is tested on RADV.
- **Performance is unproven.** The Switch's four 1 GHz Cortex-A57 cores are within a Zen 2's reach in principle, but nothing here promises frame rates before
  someone measures on the hardware. A game's tested status belongs in its note, as on the other platforms, marked as measured on a Steam Deck.

## Phases

0. **Spike: dynarmic alone — done (2026-11-26) on Linux x86_64** (`dm/linux_x64/spike`): builds dynarmic at a pinned commit and runs a guest arm64
   function (a loop, a store to guest memory, an `svc #0` the host answers by writing the sum of the bytes the guest wrote, and a `brk` that halts it),
   checking the result. Nothing of the host is linked yet. Remaining for phase 0: translation overhead against the Arm build on real game code, and the flat
   page table with fastmem over a reserved arena.
1. **The bridge (2 to 3 weeks).** The generated adapters for NVN and the host's service and SDK functions, the sentinel return for host-to-guest calls,
   threads each with a `Jit`. Check: a game reaches its frame loop with `gfx_null.c`, its log matching the Android one.
2. **The front end and Vulkan (1 to 2 weeks).** The SDL2 window (gamescope in Game Mode, X11/Wayland in Desktop Mode), the Vulkan replay on RADV, SDL2
   audio, ffmpeg movies, SDL2 input, and the launcher.
3. **Steam Deck (1 week).** Flatpak/AppImage packaging, the Non-Steam shortcut with generated artwork, suspend/resume, the 30/40 Hz defaults, and the
   Desktop Mode docs.
4. **Speed (open).** Frame time against the Arm build; dynarmic's block cache warmed at start (this repository already keeps prewarm lists); the host
   functions the game calls most bridged without adapters. Static recompilation is a project of its own, only if the JIT build is too slow — as the Windows
   plan records.

Phases 0–3 are roughly the seven to nine weeks the Windows on x64 plan estimates, more with the Linux front end and packaging written from scratch rather
than shared. Phase 0 was cheap; the bridge is the bulk.

## Risks

- **It is no longer native.** Recompiled code is slower than the game's own instructions; the Deck's power envelope may not reach the Switch's four 1 GHz
  cores once thermal limits bite.
- **The bridge is the bulk of the work.** Every host function the game can call needs an adapter generated from its prototype; varargs functions
  (`printf`-style logging in the SDK) and structures passed by value need cases written by hand. The generator is shared with the Windows on x64 build, so
  it is written once.
- **Host code that reads the game's registers** (hang reports, the sampling probe, `threads_now`, crash reports) reads them from the thread's `Jit`
  instead of a suspended thread's context, as the Windows plan notes.
- **Fixed addresses.** The host maps the guest's address space at fixed addresses: on Linux, `mmap` with `MAP_FIXED_NOREPLACE` over a large reservation,
  with the games' known ranges kept below the 48-bit user half — the Zen 2 gives that, fine, but the address check belongs in the loader.
- **Suspend mid-frame.** A submission that never completes must not hold the game's event; on pause the host drains or times out the GPU wait.
- **SteamOS updates can break the Flatpak** (a Mesa regression): the Desktop Mode folder build is the fallback, and it needs no root.
- **Testing blind.** This plan was written without a Steam Deck to run it on and without a game dump; every "supported" line above is a design assumption
  until RADV and the Deck say otherwise, and each phase says what proves what.

## Testing

Unlike the Arm builds, the x86_64 build runs on the PC that builds it: no SSH. The command-line host and the unit tests run directly; a game starts from the
launcher or the command line, with the same diagnostics files (`log.txt`, `shot_now`, `dump_now`, `threads_now`). The phase 0 spike builds and runs on any
x86_64 Linux with cmake, a C++20 compiler and Boost headers. The games themselves need the player's own dumps, as everywhere else; nothing from a game is in
this repository.
