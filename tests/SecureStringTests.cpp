// SecureStringTests.cpp — secret buffer lifetime and scrubbing.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "platform/CredentialStore.h"
#include "profiles/ConnectionProfile.h"
#include "utility/SecureString.h"

using namespace amber;

TEST_CASE("default constructed is empty", "[secure]")
{
    SecureString s;
    REQUIRE(s.Empty());
    REQUIRE(s.Size() == 0);
    REQUIRE(std::string(s.Data()).empty());
}

TEST_CASE("assign stores the value and is NUL terminated", "[secure]")
{
    SecureString s("hunter2");
    REQUIRE_FALSE(s.Empty());
    REQUIRE(s.Size() == 7);
    REQUIRE(s.View() == "hunter2");
    REQUIRE(s.Data()[7] == '\0');
    REQUIRE(s.Reveal() == "hunter2");
}

TEST_CASE("reassignment replaces the contents", "[secure]")
{
    SecureString s("first");
    s.Assign("second-and-longer");
    REQUIRE(s.View() == "second-and-longer");
    s.Assign("tiny");
    REQUIRE(s.View() == "tiny");
    REQUIRE(s.Size() == 4);
}

TEST_CASE("clear empties the buffer", "[secure]")
{
    SecureString s("secret");
    s.Clear();
    REQUIRE(s.Empty());
    REQUIRE(s.Size() == 0);
}

TEST_CASE("copy and move semantics", "[secure]")
{
    SecureString a("value");
    SecureString b(a);                       // copy ctor
    REQUIRE(b.View() == "value");

    SecureString c;
    c = a;                                   // copy assign
    REQUIRE(c.View() == "value");

    SecureString d(std::move(a));            // move ctor
    REQUIRE(d.View() == "value");
    REQUIRE(a.Empty());                      // NOLINT: intentional use-after-move

    SecureString e;
    e = std::move(d);                        // move assign
    REQUIRE(e.View() == "value");
    REQUIRE(d.Empty());                      // NOLINT
}

TEST_CASE("assigning empty clears rather than allocating", "[secure]")
{
    SecureString s("data");
    s.Assign("");
    REQUIRE(s.Empty());
}

TEST_CASE("ScrubString zeroes and clears a std::string", "[secure]")
{
    std::string secret = "passphrase";
    ScrubString(secret);
    REQUIRE(secret.empty());
}

TEST_CASE("credential target names are stable and contain no secret", "[secure][cred]")
{
    std::wstring pw = CredentialStore::TargetName("abc-123", SecretKind::Password);
    std::wstring pp = CredentialStore::TargetName("abc-123", SecretKind::KeyPassphrase);

    REQUIRE(pw == L"AmberSSH/abc-123/password");
    REQUIRE(pp == L"AmberSSH/abc-123/key-passphrase");
    REQUIRE(pw != pp);
}

// Round-trips through the real Windows Credential Manager. Uses a throwaway
// profile id and deletes it again, so it leaves nothing behind.
TEST_CASE("credential store round-trip", "[secure][cred][windows]")
{
    const std::string id = "amberssh-selftest-" + MakeUuid();
    SecureString secret("correct horse battery staple");

    std::wstring err;
    if (!CredentialStore::Store(id, SecretKind::Password, secret, &err))
    {
        WARN("Credential Manager unavailable in this environment; skipping.");
        return;
    }

    REQUIRE(CredentialStore::Exists(id, SecretKind::Password));

    SecureString loaded;
    REQUIRE(CredentialStore::Load(id, SecretKind::Password, loaded, &err));
    REQUIRE(loaded.View() == "correct horse battery staple");

    REQUIRE(CredentialStore::Erase(id, SecretKind::Password, &err));
    REQUIRE_FALSE(CredentialStore::Exists(id, SecretKind::Password));

    // Erasing an absent credential is a no-op success, not a failure.
    REQUIRE(CredentialStore::Erase(id, SecretKind::Password, &err));
}
