// MinGW CRT shim, built only for llvm-mingw (gated to MINGW in CMake).
//
// vcpkg's OpenSSL references _vsnprintf through __declspec(dllimport), so it needs __imp__vsnprintf. llvm-mingw's libmsvcrt.a has _vsnprintf only as a
// plain static symbol, which the fully static mod.dll link cannot match, so this supplies the import pointer aimed at the static function. _vsnprintf is
// pure formatting, so no CRT state crosses it.
//
// size_t and va_list are compiler builtins to keep CRT headers out of this TU.

extern "C" int _vsnprintf(char *, decltype(sizeof(0)), const char *, __builtin_va_list);
extern "C" void *__imp__vsnprintf = reinterpret_cast<void *>(&_vsnprintf);
