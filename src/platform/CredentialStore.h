// CredentialStore.h — remembered passwords and key passphrases live in the
// Windows Credential Manager, never in profiles.json.
//
// Target names:
//   AmberSSH/{profile-uuid}/password
//   AmberSSH/{profile-uuid}/key-passphrase
#pragma once

#include <string>
#include <string_view>

#include "../utility/SecureString.h"

namespace amber
{

enum class SecretKind
{
    Password,
    KeyPassphrase,
    ProxyPassword,      // AmberSSH/{profile-uuid}/proxy-password
};

class CredentialStore
{
public:
    // All three report success; failures are logged by the caller with the
    // Win32 error already translated. None of them ever log the secret.
    static bool Store(std::string_view profileId, SecretKind kind,
                      const SecureString& secret, std::wstring* errorOut = nullptr);

    static bool Load(std::string_view profileId, SecretKind kind,
                     SecureString& secretOut, std::wstring* errorOut = nullptr);

    static bool Erase(std::string_view profileId, SecretKind kind,
                      std::wstring* errorOut = nullptr);

    static bool Exists(std::string_view profileId, SecretKind kind);

    // Exposed for tests and diagnostics; contains no secret material.
    static std::wstring TargetName(std::string_view profileId, SecretKind kind);
};

} // namespace amber
