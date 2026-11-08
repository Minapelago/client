#include <frida-gum.h>

#include "pal/pal_hook.hpp"
#include "pal/pal_log.hpp"

namespace
{

bool g_initialized = false;

} // namespace

namespace pal
{

void init_hook_engine()
{
    if (g_initialized)
        return;
    gum_init_embedded();
    g_initialized = true;
    logf(LogLevel::Info, "hook_engine: frida-gum initialized");

    const auto *main_mod = gum_process_get_main_module();
    logf(LogLevel::Info, "hook_engine: main=%s", (main_mod && main_mod->name) ? main_mod->name : "?");
}

bool install_hook(void *target, void *replacement, void **trampoline)
{
    GumInterceptor *ic = gum_interceptor_obtain();
    gpointer original = nullptr;
    // The transaction holds the replacement inactive until end_transaction. replace() yields the trampoline at once, and a detour live before the
    // trampoline is published calls through null.
    gum_interceptor_begin_transaction(ic);
    const auto rc = gum_interceptor_replace(ic, target, replacement, nullptr, &original);
    if (rc != GUM_REPLACE_OK)
    {
        gum_interceptor_end_transaction(ic);
        logf(LogLevel::Error, "FridaHookEngine: replace failed at %p (rc=%d)", target, rc);
        return false;
    }
    if (trampoline)
        *trampoline = original;
    gum_interceptor_end_transaction(ic);
    return true;
}

// Reverting a target that holds no replacement is a no-op.
void remove_hook(void *target)
{
    if (target != nullptr)
        gum_interceptor_revert(gum_interceptor_obtain(), target);
}

} // namespace pal
