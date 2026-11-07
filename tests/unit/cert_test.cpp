#include <cstdlib>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "pal/pal_cert.hpp"

TEST_CASE("ca_bundle_path: honors MTHAP_AP_CERT when the file exists", "[pal][cert]")
{
    setenv("MTHAP_AP_CERT", __FILE__, 1);
    auto p = pal::ca_bundle_path();
    REQUIRE(p.has_value());
    REQUIRE(p->string() == __FILE__);
    unsetenv("MTHAP_AP_CERT");
}

TEST_CASE("ca_bundle_path: ignores MTHAP_AP_CERT when the file is missing", "[pal][cert]")
{
    // A bundle not on disk is worse than no override (curl fails on it), so the override counts only when it resolves. Only the returned path is
    // asserted; whether a system bundle exists is the host's business.
    setenv("MTHAP_AP_CERT", "/nonexistent/mthap/ca-bundle.crt", 1);
    unsetenv("SSL_CERT_FILE");
    auto p = pal::ca_bundle_path();
    REQUIRE(p != std::filesystem::path("/nonexistent/mthap/ca-bundle.crt"));
    if (p)
        REQUIRE(std::filesystem::is_regular_file(*p));
    unsetenv("MTHAP_AP_CERT");
}
