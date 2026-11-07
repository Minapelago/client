#pragma once

namespace pal
{

// Installs a process-wide crash handler that logs the faulting module+RVA and a backtrace, then lets the default handler run. Windows also writes a
// minidump to the log directory. Call once, early.
void install_crash_handler();

} // namespace pal
