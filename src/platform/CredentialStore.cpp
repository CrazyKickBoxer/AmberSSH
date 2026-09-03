#include "CredentialStore.h"

#include <vector>

#include <Windows.h>
#include <wincred.h>

namespace amber
{
namespace
{

std::wstring Widen(std::string_view text)
{
    if (text.empty())
        return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), needed);
    return wide;
}

std::wstring LastErrorText(DWORD code)
{
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = len && buffer ? std::wstring(buffer, len) : L"Unknown error";
    if (buffer)
        LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
        text.pop_back();
    return text;
}

const wchar_t* KindSuffix(SecretKind kind)
{
    switch (kind)
    {
    case SecretKind::Password:      return L"/password";
    case SecretKind::ProxyPassword: return L"/proxy-password";
    case SecretKind::KeyPassphrase: default: return L"/key-passphrase";
    }
}

} // namespace

std::wstring CredentialStore::TargetName(std::string_view profileId, SecretKind kind)
{
    return L"AmberSSH/" + Widen(profileId) + KindSuffix(kind);
}

bool CredentialStore::Store(std::string_view profileId, SecretKind kind,
                            const SecureString& secret, std::wstring* errorOut)
{
    if (profileId.empty())
        return false;

    std::wstring target = TargetName(profileId, kind);

    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target.data();
    cred.CredentialBlobSize = static_cast<DWORD>(secret.Size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.Data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = const_cast<LPWSTR>(L"AmberSSH");

    if (!CredWriteW(&cred, 0))
    {
        if (errorOut)
            *errorOut = LastErrorText(GetLastError());
        return false;
    }
    return true;
}

bool CredentialStore::Load(std::string_view profileId, SecretKind kind,
                           SecureString& secretOut, std::wstring* errorOut)
{
    secretOut.Clear();
    if (profileId.empty())
        return false;

    std::wstring target = TargetName(profileId, kind);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred))
    {
        if (errorOut)
            *errorOut = LastErrorText(GetLastError());
        return false;
    }

    if (cred->CredentialBlob && cred->CredentialBlobSize)
    {
        secretOut.Assign(std::string_view(
            reinterpret_cast<const char*>(cred->CredentialBlob),
            cred->CredentialBlobSize));
        // Scrub the copy the API handed us before releasing it.
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    }
    CredFree(cred);
    return true;
}

bool CredentialStore::Erase(std::string_view profileId, SecretKind kind,
                            std::wstring* errorOut)
{
    if (profileId.empty())
        return false;
    std::wstring target = TargetName(profileId, kind);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
    {
        DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND)
            return true;              // already absent — the desired end state
        if (errorOut)
            *errorOut = LastErrorText(code);
        return false;
    }
    return true;
}

bool CredentialStore::Exists(std::string_view profileId, SecretKind kind)
{
    if (profileId.empty())
        return false;
    std::wstring target = TargetName(profileId, kind);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return false;
    if (cred->CredentialBlob && cred->CredentialBlobSize)
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return true;
}

} // namespace amber
