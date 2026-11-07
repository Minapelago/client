// glibc_compat.h - force-included into the Dear ImGui build (Linux/x86_64).
//
// The mod is LD_PRELOADed into the game, which runs under the older Steam Linux Runtime glibc. glibc 2.41 re-versioned several libm float functions,
// so a build on a new host binds them to symbol versions the runtime lacks:
//     sqrtf, acosf, atan2f  -> @GLIBC_2.43
//     fmodf                 -> @GLIBC_2.38
//     powf                  -> @GLIBC_2.27
// The loader then rejects libmthap.so before our constructor runs: the window never opens and no log is written.
//
// ImGui's anti-aliased geometry math is the only caller. The @GLIBC_2.2.5 implementations are numerically equivalent for rendering, so every libm
// float symbol ImGui references is pinned to that version. scripts/check-glibc-max.sh enforces this at build time.
#pragma once

// This -include header is processed before any system header, so __GLIBC__ is not yet defined; guard only on compiler-predefined target macros.
// CMake applies it to non-Windows builds only.
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver powf,powf@GLIBC_2.2.5");
__asm__(".symver cosf,cosf@GLIBC_2.2.5");
__asm__(".symver sinf,sinf@GLIBC_2.2.5");
__asm__(".symver logf,logf@GLIBC_2.2.5");
__asm__(".symver ceilf,ceilf@GLIBC_2.2.5");
__asm__(".symver floorf,floorf@GLIBC_2.2.5");
#endif
