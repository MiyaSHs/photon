# Source layout and toolchain

## Toolchain: mingw-w64 GCC, C11, explicit C vtables (decided in T0.2, 2026-11-02)

Options evaluated:
- **(a) clang-cl + lld-link with no Microsoft SDK.** Rejected for now: there are no Windows headers or CRT without the MS SDK, so we would have to maintain our own declarations and import libs. Accepting the SDK EULA (xwin) is the user's decision (`NEEDS_USER.md` #2).
- **(b) mingw-w64 GCC 16.2 (headers/CRT 14.0, UCRT) writing C, not C++.** **Chosen.** The x64 Windows *C* calling convention is identical in GCC and MSVC (rcx/rdx/r8/r9, xmm0-3, 32-byte shadow space, callee-saved set), so C functions placed in explicit vtable structs interoperate with MSVC C++ virtual calls.

Rules (from `FEASIBILITY.md` §8.2):
1. **No C++ across the game ABI.** GCC's C++ member-function ABI (sret in rcx, `this` in rdx; Itanium vtables) differs from MSVC's. Game interfaces are `struct { const Vtbl *vtbl; ... }` in C.
2. **Explicit `this`:** every vtable function takes `self` as its first parameter.
3. **Methods returning a UDT** (struct or class, any size) take a hidden return pointer as the **second** parameter (after `self`) and return that same pointer, because MSVC member functions return UDTs through a hidden pointer. Scalars and pointers return normally (rax/xmm0).
4. **Calling convention:** `__attribute__((ms_abi))` is the default on this target; no annotation is needed.
5. **No runtime dependencies:** `-static-libgcc`, with UCRT/system DLLs only (see the toolchain file).

Verified by `tests/smoke` under Wine 11.20:
- our DLL works through C vtables (including a UDT return through the hidden pointer);
- the stock MSVC-built `shaderapidx9.dll` loads, and the vtables of its 5 interfaces sit at base+0x7cf90 / 0x788c0 / 0x7b4b8 / 0x7b368 / 0x7e250;
- vtable calls into MSVC code return the expected values (`HWConfig` slot 24 = field +0x54c).

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build
source scripts/env.sh && (cd build/tests/smoke && wine smoke.exe "$(winepath -w "$GMOD_BIN")")
```
