#include <cstdlib>
#include <fstream>
#include <string>

#include "pal/pal_cert.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <wincrypt.h>

namespace fs = std::filesystem;

namespace
{

// PEM block with CRLF line breaks, which OpenSSL's PEM reader accepts.
std::string der_to_pem(const unsigned char *data, DWORD len)
{
    DWORD size = 0;
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64HEADER, nullptr, &size))
        return {};
    std::string out(size, '\0');
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64HEADER, out.data(), &size))
        return {};
    out.resize(size);
    return out;
}

fs::path out_path()
{
    if (const char *appdata = std::getenv("LOCALAPPDATA"); appdata && *appdata)
        return fs::path(appdata) / "mth-apclient" / "system-ca.pem";
    return fs::temp_directory_path() / "mth-apclient" / "system-ca.pem";
}

} // namespace

namespace pal
{

std::optional<fs::path> ca_bundle_path()
{
    if (const char *o = std::getenv("MTHAP_AP_CERT"); o && *o)
    {
        std::error_code ec;
        if (fs::exists(o, ec))
            return fs::path(o);
    }

    // Built once; the MTHAP_AP_CERT override is still checked per call.
    static const std::optional<fs::path> cached = []() -> std::optional<fs::path>
    {
        HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
        if (!store)
            return std::nullopt;

        const fs::path out = out_path();
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            CertCloseStore(store, 0);
            return std::nullopt;
        }

        int count = 0;
        PCCERT_CONTEXT ctx = nullptr;
        while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr)
        {
            const std::string pem = der_to_pem(ctx->pbCertEncoded, ctx->cbCertEncoded);
            if (pem.empty())
                continue;
            f << pem;
            ++count;
        }
        CertCloseStore(store, 0);
        f.flush();
        const bool ok = f.good();
        f.close();

        if (count > 0 && ok)
            return out;
        return std::nullopt;
    }();
    return cached;
}

} // namespace pal
