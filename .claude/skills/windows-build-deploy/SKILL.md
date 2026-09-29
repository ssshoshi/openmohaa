---
name: windows-build-deploy
description: Cross-compile openmohaa (or just a renderer DLL) for Windows from this Linux/WSL host via mingw-w64, and deploy the result into the user's real Windows game install for live testing. Use whenever the user asks to "build for windows", "test on windows", "copy it over" after Windows-targeted changes, or reports something only reproducible on their actual Windows machine.
---

# Windows build + deploy workflow

This project is normally built and tested on Linux (`build/gl2`, `build/full`, etc). When a
change needs verifying on real Windows/hardware (GPU-specific rendering bugs, driver behavior,
performance), cross-compile with mingw-w64 and copy the result straight into the user's actual
game install under the WSL `/mnt/c` or `/mnt/d` mount — no separate Windows toolchain or reboot
needed.

## 0. Find out what you're targeting

Ask the user (if not already known from context):
- Their game install directory (a WSL path like `/mnt/d/Medal of Honor/<install-name>/`, found
  via `find /mnt/c /mnt/d -maxdepth 3 -iname "openmohaa.exe" 2>/dev/null` if unknown).
- Their homepath, where configs/logs/screenshots live — almost always
  `/mnt/c/Users/<username>/AppData/Roaming/openmohaa/main/` (confirm the username segment).
- Whether they only need a renderer DLL rebuilt (`renderer_opengl2.dll` /
  `renderer_opengl1.dll`) — the common case, since the shipped `openmohaa.exe` already exists in
  their install — or a full engine rebuild.

## 1. Check the REF_API_VERSION matches their exe

The renderer talks to the client through a small versioned interface
(`code/renderercommon/tr_public.h`, `REF_API_VERSION`). If the user's installed `openmohaa.exe`
was built from a different branch than the one you're working in, the versions can drift and the
game will refuse to load your freshly built renderer DLL with an error like:

```
Trying to load "renderer_opengl2.dll" from "."...
Mismatched REF_API_VERSION: expected 14, got 15
```

Before building, check whether this matters:

```bash
grep -n REF_API_VERSION code/renderercommon/tr_public.h
```

If you don't know what branch their `openmohaa.exe` was built from, ask, or just build and let
the mismatch (if any) surface — it's a clear, unambiguous error naming both version numbers.

**If there's a mismatch**, don't fight it in the main tree. Build against the branch that
actually matches their exe, in a disposable worktree, with your changes layered on top:

```bash
git worktree add --detach "$CLAUDE_SCRATCHPAD/win-int" <branch-that-matches-their-exe>
cd "$CLAUDE_SCRATCHPAD/win-int"

# Bring your actual changes in. If they're committed on your branch, cherry-pick the specific
# commits. If they're still uncommitted in the main tree, take a diff and apply it instead:
git -C /home/shoshi/projects/openmohaa diff <base>..HEAD -- code/renderergl2 | git apply --3way

# Or for uncommitted changes in the main tree:
git -C /home/shoshi/projects/openmohaa diff -- code/renderergl2 > /tmp/changes.diff
git apply --3way /tmp/changes.diff
```

Expect a handful of trivial conflicts if the target branch has independently fixed the same
Windows-portability issues (see step 2) — resolve with `git checkout --ours <file>` for those,
and keep every real behavioral change intact. From here on, run every command in this worktree,
not the main tree, and remember to **copy every subsequent edit into the worktree too** before
rebuilding (`cp code/renderergl2/tr_backend.c "$WT/code/renderergl2/tr_backend.c"` etc, or just
re-diff-and-apply) — the worktree is a separate checkout, edits in the main tree don't appear
there automatically.

If the versions already match, skip all of this and just build in the main tree directly.

## 2. Toolchain file

Check whether `cmake/toolchains/mingw-w64-x86_64.cmake` already exists (it may be untracked —
this file has a habit of not making it into commits). If missing, create it:

```cmake
# Cross compile Windows x86_64 binaries from a Unix host using mingw-w64.
#
#   cmake -S . -B build/win64 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
#       -DBUILD_RENDERER_GL2=ON -DUSE_RENDERER_DLOPEN=ON
#
# The Windows SDL2 import libraries and DLL are already in the tree under
# code/thirdparty/libs/win64, so USE_INTERNAL_SDL (on by default) is enough and
# no Windows SDK or prebuilt dependency tree is needed.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TARGET x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${MINGW_TARGET}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_TARGET}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_TARGET}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TARGET})

# Link the GCC and C++ runtimes in, so the binaries do not need
# libgcc_s_seh-1.dll and libstdc++-6.dll sitting next to them. This matches how
# the shipped Windows binaries behave -- they import only SDL2, KERNEL32 and
# msvcrt -- and keeps a renderer DLL a drop-in file on its own.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Look for headers and libraries in the target tree, but run build tools such as
# flex and bison from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

If `x86_64-w64-mingw32-gcc` isn't installed: `sudo apt install mingw-w64` (Debian/Ubuntu/WSL).

## 3. Configure (first time only per build directory)

```bash
cmake -S . -B build/win64 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
    -DBUILD_RENDERER_GL2=ON -DUSE_RENDERER_DLOPEN=ON
```

Add `-DBUILD_RENDERER_GL1=ON` too if GL1 is also needed. `USE_RENDERER_DLOPEN=ON` is required —
it's what makes the renderer a standalone, swappable DLL rather than something statically linked
into the client, which is what lets you replace just the renderer without touching the exe at
all.

## 4. Build

Renderer-only (the common case — fast, and all that's usually needed):

```bash
cmake --build build/win64 --target renderer_opengl2 --parallel
```

Output lands at `build/win64/RelWithDebInfo/renderer_opengl2.dll` (the config subdirectory name
appears even though this is a single-config Makefile generator, not multi-config — that's just
how the build's output directories are set up).

Full engine (only if actually needed — e.g. testing an engine-level, not renderer-level, change):

```bash
cmake --build build/win64 --parallel
```

**Known Windows-portability fixups**, already applied in this tree if you're on a branch that
has done this before (check before re-fixing from scratch — grep for the symptom first):

- `code/qcommon/q_platform.h` — `alloca` include guard must check
  `defined(_MSC_VER) || defined(_WIN32)`, not just `_MSC_VER`, or MinGW hits a nonexistent
  `<alloca.h>` fallback.
- `code/qcommon/files.cpp` — needs a `#elif defined(__MINGW32__)` branch pulling in
  `<sys/types.h>`/`<sys/stat.h>` for `struct stat`, alongside the existing `_WIN32`/other guards.
- `code/sys/new/sys_win32_new.c`, `code/Launcher/launch_win32.cpp` — `#include <Windows.h>` must
  be lowercase `<windows.h>` (case-sensitive filesystem on the Linux host).
- `code/gamespy/common/gsMemory.c` — `#if (_MSC_VER < 1300)` must be
  `#if defined(_MSC_VER) && (_MSC_VER < 1300)`, or it's true when `_MSC_VER` is undefined under
  MinGW and conflicts with a real declaration.
- `cmake/compilers/gnu.cmake` — `-lrt` must be gated `NOT APPLE AND NOT WIN32`, not just
  `NOT APPLE`, or the MinGW link fails.
- `cmake/platforms/windows.cmake` — inside the `if(MINGW)` block, add
  `string(APPEND CMAKE_RC_FLAGS " -I\"${CMAKE_SOURCE_DIR}\"")` so windres can resolve the icon
  path from the build directory.

If you hit a *silent* renderer load failure (client falls back to the other renderer with no
build error) rather than a crash, suspect C++/C linkage: anything using `extern "C"` functions
from a header that's included in a C++ translation unit *before* the `extern "C" { ... }` block
opens will get C++ name mangling and the DLL will have unresolved symbols at `dlopen()` time
only, not link (or even always) time. `-Wl,--no-undefined` on the renderer link target turns this
into a build failure instead, which is far easier to catch — check it's present in
`cmake/renderer_common.cmake` and add it (guarded by `check_linker_flag`, skip on Apple/MSVC) if
not.

## 5. Deploy

The game must not be running — copying over a loaded DLL fails with a permission error. Ask the
user to close it first if the copy fails.

```bash
cp build/win64/RelWithDebInfo/renderer_opengl2.dll "<their-install-dir>/renderer_opengl2.dll"
```

Then ask them to relaunch (or if already running with the old DLL and they just closed it,
relaunch) and reproduce whatever you're testing.

## 6. Getting feedback back

The user can't easily paste console output, so lean on their `qconsole.log` and screenshots
instead of asking them to transcribe things:

- **Console log**: ask them to run `/logfile 2` and `/developer 1` once (persists in their
  config), then just read `<homepath>/main/qconsole.log` yourself after they reproduce something
  — no need to ask them to copy/paste. It's appended to across launches, so when re-checking after
  a new repro, look at the tail / lines after the last known-good timestamp, not the whole file.
- **Screenshots**: ask them to run `/screenshotJPEG` (or bind a key to it —
  `/bind F12 screenshotJPEG` — if they need one taken without opening the console, since the
  console itself will otherwise appear in the shot) and tell you the filename; read it directly
  with the Read tool from `<homepath>/main/screenshots/shot####.jpg`.
- **Their saved config**: `<homepath>/main/configs/omconfig.cfg` is worth checking early when
  behavior looks wrong and doesn't match a fresh default — it's `CVAR_ARCHIVE` and persists
  across every relaunch, so stale values from earlier testing sessions (before a cvar's default
  changed, or from a one-off experiment) silently override new code defaults. This has caused
  real, time-consuming misdiagnoses on this project before — see
  `gl2-stale-omconfig-cvars` in the assistant's project memory if present.

## 7. Iterating

Every subsequent code change: edit in the main tree as normal, `cp` the changed file(s) into the
worktree if you're using one (step 1), re-run the build command from step 4, re-copy the DLL
(step 5), ask for a fresh repro. Diagnostic instrumentation (temporary `ri.Printf` traces, debug
overlays) is often the fastest way to see what's actually happening on hardware you can't drive
yourself — add it, deploy, read the log, then **remember to strip it back out** once the question
it answered is settled; grep for whatever marker you tagged it with (e.g. `TRACE:`, `VERIFY:`)
across the renderer source before considering the investigation done.
