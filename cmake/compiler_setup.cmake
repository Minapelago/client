# compiler_setup.cmake - warning flags and release debug info. Directory scope: it applies to the
# targets created after the include, so third-party targets declared earlier keep their own flags.
# Only clang (GNU frontend, including llvm-mingw) and clang-cl are supported compilers.

add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wshadow;-Wno-missing-field-initializers;-Wno-unused-parameter>"
    # clang-cl is noisy with C++ back-compat diagnostics that are irrelevant to a C++23 project.
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-c++98-compat;-Wno-c++98-compat-pedantic;-Wno-c++17-compat>"
)

# Release ships debug info.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # /Z7 keeps CodeView in the objects rather than a per-TU PDB (no writer contention under ninja).
    # CMAKE_MSVC_DEBUG_INFORMATION_FORMAT is left alone: its Embedded value maps to -g, which
    # would not combine with -gline-tables-only predictably, and its default covers Debug.
    # /DEBUG makes the linker emit the single mod.pdb.
    add_compile_options("$<$<CONFIG:Release>:/Z7;-gline-tables-only>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
else()
    # No thin-LTO: it turns the static libs into LLVM-bitcode archives that a
    # non-LTO consumer linked with GNU ld can't read ("file format not
    # recognized"), and the gain is negligible at this size. If LTO is wanted
    # later, add -flto=thin here and -fuse-ld=lld to the link (and install lld
    # in CI) so every link is LTO-aware.
    add_compile_options("$<$<CONFIG:Release>:-gline-tables-only;-gz=zlib>")
    add_link_options("$<$<CONFIG:Release>:-gz=zlib;LINKER:--compress-debug-sections=zlib>")
endif()
