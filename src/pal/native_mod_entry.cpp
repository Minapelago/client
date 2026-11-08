#include <cstdlib>
#include <exception>

#include "MinaModAPI.h"
#include "mod/mod_api.hpp"
#include "pal/pal_crash.hpp"
#include "pal/pal_entry.hpp"
#include "pal/pal_log.hpp"

// Entry point the native mod loader calls on the game thread, before Game::Init and before any World exists. MM_EXPORT carries the extern "C" linkage and
// the default-visibility attribute the hidden-by-default build needs.
MM_EXPORT void MinaMod_Init(MinaModAPI *mm)
{
    mod::set_api(mm);
    pal::log_init();
    pal::install_crash_handler(); // before any mod work, so it catches everything
    if (mm && mm->APIVersion != MinaModAPI_Version)
        pal::logf(pal::LogLevel::Warn, "MinaModAPI version mismatch (expected %u, got %zu); proceeding anyway", static_cast<unsigned>(MinaModAPI_Version),
                  static_cast<size_t>(mm->APIVersion));
    // An exception unwinding out of this extern "C" entry into the game's frames can fast-fail past the crash handler, so log it before aborting.
    try
    {
        pal::apclient_main();
    }
    catch (const std::exception &e)
    {
        pal::logf(pal::LogLevel::Error, "apclient startup threw: %s", e.what());
        std::abort();
    }
}
