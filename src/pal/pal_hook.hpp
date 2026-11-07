#pragma once

namespace pal
{

// Frida-Gum on Linux, MinHook on Windows. A hook is identified by its target address: both backends
// refuse a second hook on a target that is already hooked. install_hook publishes the trampoline
// before the detour goes live and returns false (after logging) when the backend refuses.
bool install_hook(void *target, void *replacement, void **trampoline);
void remove_hook(void *target);

void init_hook_engine();
void shutdown_hook_engine();

} // namespace pal
