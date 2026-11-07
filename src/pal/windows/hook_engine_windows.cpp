#include "pal/pal_hook.hpp"
#include "pal/pal_log.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <MinHook.h>

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
    MH_STATUS rc = MH_Initialize();
    if (rc == MH_OK || rc == MH_ERROR_ALREADY_INITIALIZED)
    {
        g_initialized = true;
        logf(LogLevel::Info, "hook_engine: MinHook initialized (%s)", MH_StatusToString(rc));
    }
    else
    {
        logf(LogLevel::Error, "hook_engine: MH_Initialize failed (%s)", MH_StatusToString(rc));
    }
}

void shutdown_hook_engine()
{
    if (!g_initialized)
        return;
    MH_STATUS rc = MH_Uninitialize();
    g_initialized = false;
    logf(LogLevel::Info, "hook_engine: MinHook shutdown (%s)", MH_StatusToString(rc));
}

bool install_hook(void *target, void *replacement, void **trampoline)
{
    void *original = nullptr;
    const auto s_create = MH_CreateHook(target, replacement, &original);
    if (s_create != MH_OK)
    {
        logf(LogLevel::Error, "MinHookEngine: CreateHook failed at %p (%s)", target, MH_StatusToString(s_create));
        return false;
    }
    // Publish before enabling. EnableHook makes the replacement callable from other threads
    // immediately, and every replacement calls through its trampoline; publishing afterwards
    // leaves a window where the detour is live and the trampoline is still null.
    if (trampoline)
        *trampoline = original;
    const auto s_enable = MH_EnableHook(target);
    if (s_enable != MH_OK)
    {
        if (trampoline)
            *trampoline = nullptr;
        MH_RemoveHook(target);
        logf(LogLevel::Error, "MinHookEngine: EnableHook failed at %p (%s)", target, MH_StatusToString(s_enable));
        return false;
    }
    return true;
}

// MH_ALL_HOOKS is null, so a null target must not reach MH_DisableHook or it would disable every hook.
// A target that holds no hook returns MH_ERROR_NOT_CREATED, which is ignored.
void remove_hook(void *target)
{
    if (target == nullptr)
        return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
}

} // namespace pal
