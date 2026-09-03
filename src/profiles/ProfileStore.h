// ProfileStore.h — loads and atomically saves profiles.json. A malformed entry
// is skipped and reported; it never discards the whole file.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ConnectionProfile.h"

namespace amber
{

class ProfileStore
{
public:
    struct LoadReport
    {
        int schemaVersion = ConnectionProfile::kSchemaVersion;
        int skipped = 0;             // malformed entries that were dropped
        std::string fileError;       // non-empty => the file itself failed
    };

    bool Load(LoadReport* reportOut = nullptr);
    bool Save(std::string* errorOut = nullptr) const;

    // Explicit-path variants, used by the unit tests.
    bool LoadFrom(const std::filesystem::path& file, LoadReport* reportOut = nullptr);
    bool SaveTo(const std::filesystem::path& file, std::string* errorOut = nullptr) const;

    const std::vector<ConnectionProfile>& All() const { return m_profiles; }
    std::size_t Count() const { return m_profiles.size(); }
    void Clear() { m_profiles.clear(); }

    const ConnectionProfile* Find(std::string_view id) const;
    ConnectionProfile* Find(std::string_view id);
    const ConnectionProfile* FindByName(std::string_view name) const;

    // Inserts, or replaces the entry with a matching id. Returns the id used.
    std::string Upsert(ConnectionProfile profile);
    bool Remove(std::string_view id);

private:
    std::vector<ConnectionProfile> m_profiles;
};

} // namespace amber
