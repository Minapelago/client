#pragma once

#include <cstdarg>
#include <filesystem>
#include <functional>
#include <string_view>

namespace pal
{

enum class LogLevel
{
    Debug,
    Info,
    Warn,
    Error
};

class ILog
{
  public:
    virtual ~ILog() = default;
    virtual void write(LogLevel, std::string_view message) = 0;
};

void log_init(std::string_view stem = "mthap");

// printf format checking. Clang only knows the printf archetype (it ignores gnu_printf) and that accepts %zu; GCC needs gnu_printf for C99 specifiers.
#if defined(__clang__)
#define MTHAP_PRINTF_FORMAT(fmt, va) __attribute__((format(printf, fmt, va)))
#elif defined(__GNUC__)
#define MTHAP_PRINTF_FORMAT(fmt, va) __attribute__((format(gnu_printf, fmt, va)))
#else
#define MTHAP_PRINTF_FORMAT(fmt, va)
#endif

void logf(LogLevel, const char *fmt, ...) MTHAP_PRINTF_FORMAT(2, 3);
void vlogf(LogLevel, const char *fmt, std::va_list ap);

std::filesystem::path log_dir();

// File descriptor of the active log, or -1. The Linux crash handler writes its backtrace through it with async-signal-safe calls only (no logf or
// malloc); Windows returns -1 since its handler uses logf and a minidump.
int log_fd();

ILog &default_log();
void set_default_log(ILog *);

// The observer runs under the log mutex and must not call logf/vlogf (deadlock).
using LogObserver = std::function<void(LogLevel, std::string_view)>;
void set_log_observer(LogObserver);

} // namespace pal
