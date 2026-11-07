#include <thread>

#include "MinaModAPI.h"
#include "mod/mod_api.hpp"
#include "pal/pal_crash.hpp"
#include "pal/pal_entry.hpp"
#include "pal/pal_log.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{

#ifdef _WIN32
// Raw CreateThread rather than std::thread: the MSVC STL thread wrapper is noexcept, so an exception
// escaping apclient_main would reach terminate and __fastfail, skipping the crash handler's filter
// (no log line, no minidump). A plain thread proc lets it reach RtlUserThreadStart's filter.
DWORD WINAPI apclient_thread_proc(LPVOID)
{
    SetThreadDescription(GetCurrentThread(), L"mthap-main");
    pal::apclient_main();
    return 0;
}
#endif

void start_apclient_thread()
{
#ifdef _WIN32
    HANDLE h = CreateThread(nullptr, 0, &apclient_thread_proc, nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
    else
        pal::logf(pal::LogLevel::Error, "failed to start the apclient thread (error %lu)", static_cast<unsigned long>(GetLastError()));
#else
    try
    {
        std::thread(
            []
            {
                pthread_setname_np(pthread_self(), "mthap-main");
                pal::apclient_main();
            })
            .detach();
    }
    catch (const std::system_error &e)
    {
        pal::logf(pal::LogLevel::Error, "failed to start the apclient thread: %s", e.what());
    }
#endif
}

} // namespace

// Entry point the native mod loader calls. MM_EXPORT carries the extern "C"
// linkage and the default-visibility attribute the hidden-by-default build needs.
MM_EXPORT void MinaMod_Init(MinaModAPI *mm)
{
    mod::set_api(mm);
    pal::log_init();
    pal::install_crash_handler(); // before any mod work, so it catches everything
    if (mm && mm->APIVersion != MinaModAPI_Version)
        pal::logf(pal::LogLevel::Warn, "MinaModAPI version mismatch (expected %u, got %zu); proceeding anyway", static_cast<unsigned>(MinaModAPI_Version),
                  static_cast<size_t>(mm->APIVersion));
    start_apclient_thread();
}
