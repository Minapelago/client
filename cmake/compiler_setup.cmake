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
    # /Z7 keeps CodeView in the objects (no per-TU PDB, so no writer contention under ninja). /DEBUG makes the linker emit the single mod.pdb.
    # CMAKE_MSVC_DEBUG_INFORMATION_FORMAT is left alone: its Embedded value maps to -g, which combines unpredictably with -gline-tables-only.
    add_compile_options("$<$<CONFIG:Release>:/Z7;-gline-tables-only>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
else()
    # No thin-LTO: it makes the static libs LLVM-bitcode archives that a non-LTO consumer linked with GNU ld can't read. To enable it, add
    # -flto=thin here and -fuse-ld=lld to the link (and install lld in CI).
    add_compile_options("$<$<CONFIG:Release>:-gline-tables-only;-gz=zlib>")
    add_link_options("$<$<CONFIG:Release>:-gz=zlib;LINKER:--compress-debug-sections=zlib>")
endif()
