#include <cstdlib>
#include <string>

#include "pal/pal_cert.hpp"

namespace fs = std::filesystem;

namespace
{

std::optional<fs::path> env_path(const char *var)
{
    if (const char *v = std::getenv(var); v && *v)
    {
        std::error_code ec;
        if (fs::exists(v, ec))
            return fs::path(v);
    }
    return std::nullopt;
}

} // namespace

namespace pal
{

std::optional<fs::path> ca_bundle_path()
{
    if (auto p = env_path("MTHAP_AP_CERT"))
        return p;
    if (auto p = env_path("SSL_CERT_FILE"))
        return p;

    static const char *const kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Steam sniper
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL
        "/etc/ssl/cert.pem",                  // Alpine/BSD
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/tls/cacert.pem",
    };
    // Steam's pressure-vessel container ships its own trust store, which can lag the host's by years (the sniper runtime lacks the SSL.com 2022 roots),
    // and its opt-in --import-ca-certs fails on hosts without hashed symlinks in /etc/ssl/certs. The host filesystem is at /run/host, so read its store
    // first; outside a container the prefixed paths do not exist.
    static const char *const kHostPrefixes[] = {"/run/host", ""};
    std::error_code ec;
    for (const char *prefix : kHostPrefixes)
        for (const char *cand : kCandidates)
            if (fs::path p = std::string(prefix) + cand; fs::exists(p, ec))
                return p;
    return std::nullopt;
}

} // namespace pal
