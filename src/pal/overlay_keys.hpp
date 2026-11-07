#pragma once

#include <charconv>
#include <string_view>

namespace pal
{

// MTHAP_CONSOLE_KEY / MTHAP_LOGIN_KEY string to a platform key code. F1..F12 are contiguous in SDL keycodes and Win32 virtual keys, so the caller passes
// F1 and backquote. Unset or unrecognised gives F1.
template <class Key> Key parse_console_key(const char *name, Key f1, Key backquote)
{
    if (name == nullptr)
        return f1;
    const std::string_view sv{name};
    if (sv == "BACKQUOTE" || sv == "GRAVE" || sv == "`")
        return backquote;
    if (sv.size() >= 2 && sv[0] == 'F' && sv[1] != '0')
    {
        int n = 0;
        const auto [end, ec] = std::from_chars(sv.data() + 1, sv.data() + sv.size(), n);
        if (ec == std::errc{} && end == sv.data() + sv.size() && n >= 1 && n <= 12)
            return static_cast<Key>(f1 + (n - 1));
    }
    return f1;
}

// Falls back to F2 when the login key would collide with the console key (the default with MTHAP_LOGIN_KEY unset).
template <class Key> Key parse_login_key(const char *name, Key toggle, Key f1, Key backquote)
{
    const Key k = parse_console_key(name, f1, backquote);
    return k == toggle ? static_cast<Key>(f1 + 1) : k;
}

} // namespace pal
