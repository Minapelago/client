#include "mth/app/app.hpp"
#include "pal/pal_entry.hpp"

namespace pal
{

void apclient_main()
{
    // Leaked, with no unload path: neither hook backend drains threads already inside a detour, so reverting one while the game runs crashes. App keeps its
    // own pointer for the tick trampolines.
    new mth::App();
}

} // namespace pal
