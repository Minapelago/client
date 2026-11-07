#pragma once

#include <cstdint>

#include "pal/pal_hook.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_module.hpp"

namespace pal
{

// RAII detour on a game function. A failed install leaves the hook inert (installed() == false);
// destruction or reset() removes it. Replacement and trampoline stay file-scope in the owning module
// (Frida replacements have no user context); this type owns only the installed target.
//
// The App is leaked by design, so a namespace-scope ScopedHook has to be declared [[clang::no_destroy]]
// and removed through reset(). Its exit-time destructor would otherwise revert detours that game
// threads may still be running.
class ScopedHook
{
  public:
    ScopedHook() = default;

    // Installs at an already-resolved address. Apart from the backend's own failure line nothing is
    // logged, so the caller reports the outcome. A zero address leaves the hook inert.
    template <class Fn> ScopedHook(std::uintptr_t target, Fn *replacement, Fn **trampoline)
    {
        void *addr = reinterpret_cast<void *>(target);
        if (addr != nullptr && install_hook(addr, reinterpret_cast<void *>(replacement), reinterpret_cast<void **>(trampoline)))
            target_ = addr;
    }

    // Resolves symbol and logs the outcome under label.
    template <class Fn> ScopedHook(const char *symbol, Fn *replacement, Fn **trampoline, const char *label)
    {
        const std::uintptr_t addr = resolve_game_symbol(symbol);
        if (addr == 0)
        {
            logf(LogLevel::Warn, "hook: symbol %s (%s) not found; not hooked", symbol, label);
            return;
        }
        *this = ScopedHook(addr, replacement, trampoline);
        if (installed())
            logf(LogLevel::Info, "hook: hooked %s at %p", label, target());
        else
            logf(LogLevel::Error, "hook: failed to hook %s at %p", label, reinterpret_cast<void *>(addr));
    }

    ~ScopedHook()
    {
        reset();
    }

    ScopedHook(ScopedHook &&other) noexcept : target_(other.target_)
    {
        other.target_ = nullptr;
    }

    ScopedHook &operator=(ScopedHook &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            target_ = other.target_;
            other.target_ = nullptr;
        }
        return *this;
    }

    ScopedHook(const ScopedHook &) = delete;
    ScopedHook &operator=(const ScopedHook &) = delete;

    [[nodiscard]] bool installed() const
    {
        return target_ != nullptr;
    }

    // The hooked address, or null when not installed.
    [[nodiscard]] void *target() const
    {
        return target_;
    }

    void reset()
    {
        if (target_ != nullptr)
            remove_hook(target_);
        target_ = nullptr;
    }

  private:
    void *target_{nullptr};
};

} // namespace pal
