#pragma once

namespace pal
{

// Frida-Gum on Linux, MinHook on Windows; both refuse a second hook on one target. install_hook publishes the trampoline before the detour goes live and
// logs and returns false on refusal.
bool install_hook(void *target, void *replacement, void **trampoline);
void remove_hook(void *target);

void init_hook_engine();

} // namespace pal
