#include "mth/app/app.hpp"
#include "pal/pal_entry.hpp"

namespace pal
{

void apclient_main()
{
    // Intentionally leaked, and the mod exports no unload path: neither hook backend drains threads already
    // inside a detour, so reverting one while the game runs is a crash footgun. ~App and its teardown chain
    // (HookManager, the feature hooks, their hook removals) is kept correct but never runs today. The App
    // keeps its own pointer for the tick trampolines.
    new mth::App();
}

} // namespace pal
