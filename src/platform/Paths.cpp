#include "Paths.h"

#include <stdexcept>

#include <Windows.h>
#include <ShlObj.h>

namespace amber
{
namespace
{

std::filesystem::path ResolveRoot()
{
    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                      nullptr, &raw);
    if (FAILED(hr) || !raw)
    {
        if (raw)
            CoTaskMemFree(raw);
        throw std::runtime_error("Unable to resolve %LOCALAPPDATA%.");
    }
    std::filesystem::path root(raw);
    CoTaskMemFree(raw);
    root /= L"AmberSSH";

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
        throw std::runtime_error("Unable to create the AmberSSH data folder: " +
                                 ec.message());
    return root;
}

} // namespace

const std::filesystem::path& DataRoot()
{
    static const std::filesystem::path root = ResolveRoot();
    return root;
}

std::filesystem::path ProfilesFile() { return DataRoot() / L"profiles.json"; }
std::filesystem::path SettingsFile() { return DataRoot() / L"settings.json"; }
std::filesystem::path KnownHostsFile() { return DataRoot() / L"known_hosts"; }

std::filesystem::path LogDirectory()
{
    std::filesystem::path dir = DataRoot() / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace amber
