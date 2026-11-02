# Third-party code and licences

This repository ships source code only. The build pulls or links the parts below.

| Part | Where | Licence | Notes |
|---|---|---|---|
| xboxrecomp (sp00nznet) | `xboxrecomp/` | MIT (`xboxrecomp/LICENSE`) | Vendored, with changes by antoxa2584x and this port |
| xemu MCPX APU code, `nv2a_regs.h` | `xboxrecomp/src/apu/`, `xboxrecomp/src/nv2a/` | LGPL-2.1-or-later | Details and full text: `xboxrecomp/NOTICE`, `xboxrecomp/LICENSES/` |
| flatout2-sw game layer (antoxa2584x) | `src/`, `CMakeLists.txt`, `switch/`, `tools/` | MIT (root `LICENSE` of flatout2-sw) | This port began as a copy of flatout2-sw @ 375edee |
| Mesa NVK, mesa-switch (danfromtico) | built by `tools/build_nvk.sh` at 0ff5207 | MIT (Mesa; see the checkout's `docs/license.rst`) | Linked into `clx-vulkan.nro` |
| glslang 15.4.0 (Khronos) | built by `tools/build_glslang_switch.sh` | BSD-3-Clause and others (its `LICENSE.txt`) | Linked into `clx-vulkan.nro` |
| libnx, devkitA64 (devkitPro) | devkitPro install | ISC (libnx); GPL toolchain | |
| SDL2 for Switch | devkitPro `switch-sdl2` | zlib | |
| Mesa for Switch (GL), libdrm_nouveau, glad, zlib, zstd, expat | devkitPro portlibs | MIT / zlib / BSD | `clx.nro` (GL) links Mesa |
| capstone (Python) | build host only | BSD-3-Clause | Disassembler used by the lifter |
| Pillow (Python) | build host only | MIT-CMU | Icon and logo from the XBE |
| SDL2 | Linux and Windows builds (not shipped; `clx.exe` is packaged with `SDL2.dll` from MSYS2) | zlib | Window, GL context, audio (Linux), gamepad (Linux) |
| MSYS2 UCRT64: clang/LLVM, mingw-w64 runtime, winpthreads | Windows build host; `libwinpthread-1.dll` is copied into `out/windows` | Apache-2.0 with LLVM exception; ZPL/MIT | Compiler and runtime DLLs |
| OpenSSL, libepoxy | Linux build and run (system libraries) | Apache-2.0; MIT | Not bundled |
| live-and-recompiled (giacomozama, TGP482; Mabec-games branch `launcher`) | reference only, no code in this repository | GPL-3.0 | Another port of the same game. Its addresses and findings guided the `vignette`, `grass_wind` and `language` options (`src/recomp_manual.c`); no code was copied |

The game itself (code, art, audio, text) belongs to its owners. Nothing from it is in
this repository: `build_from_xbe.sh` reads your `default.xbe` on your machine.
