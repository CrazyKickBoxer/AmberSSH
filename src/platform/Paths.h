// Paths.h — per-user storage locations under %LOCALAPPDATA%\AmberSSH.
#pragma once

#include <filesystem>

namespace amber
{

// Root directory, created on first call. Throws std::runtime_error if the
// known folder cannot be resolved or the directory cannot be created.
const std::filesystem::path& DataRoot();

std::filesystem::path ProfilesFile();   // profiles.json
std::filesystem::path SettingsFile();   // settings.json
std::filesystem::path KnownHostsFile(); // known_hosts
std::filesystem::path LogDirectory();   // logs\

} // namespace amber
