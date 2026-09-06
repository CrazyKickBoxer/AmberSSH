// CredentialStoreTests.cpp — where remembered passwords actually live.
//
// These touch the real Windows Credential Manager, because that IS the thing
// under test and a fake would only prove the fake works. Every case uses an
// id that names itself as a test, and erases before and after, so a failed
// run cannot leave a credential behind under a name that looks real.
#include <catch2/catch_test_macros.hpp>

#include "../src/platform/CredentialStore.h"
#include "../src/utility/SecureString.h"

using amber::CredentialStore;
using amber::SecretKind;
using amber::SecureString;

namespace
{
// Deliberately unmistakable: if this is ever seen in Credential Manager, it is
// litter from a test run and not one of the user's saved connections.
const char* const kTestId = "amberssh-unit-test-DO-NOT-USE-00000000";

struct Scrub
{
    Scrub() { Erase(); }
    ~Scrub() { Erase(); }
    static void Erase()
    {
        for (SecretKind k : { SecretKind::Password, SecretKind::KeyPassphrase,
                              SecretKind::ProxyPassword })
            CredentialStore::Erase(kTestId, k);
    }
};
}

TEST_CASE("a stored secret comes back exactly", "[credstore]")
{
    Scrub guard;
    const std::string secret = "correct horse battery staple \x01\x02 \xC3\xA9";
    REQUIRE(CredentialStore::Store(kTestId, SecretKind::Password, SecureString(secret)));
    REQUIRE(CredentialStore::Exists(kTestId, SecretKind::Password));

    SecureString out;
    REQUIRE(CredentialStore::Load(kTestId, SecretKind::Password, out));
    REQUIRE(out.Size() == secret.size());
    REQUIRE(std::string(out.Data(), out.Size()) == secret);
}

TEST_CASE("the three kinds do not collide", "[credstore]")
{
    Scrub guard;
    REQUIRE(CredentialStore::Store(kTestId, SecretKind::Password, SecureString("pw")));
    REQUIRE(CredentialStore::Store(kTestId, SecretKind::KeyPassphrase, SecureString("pass")));
    REQUIRE(CredentialStore::Store(kTestId, SecretKind::ProxyPassword, SecureString("proxy")));

    SecureString a, b, c;
    REQUIRE(CredentialStore::Load(kTestId, SecretKind::Password, a));
    REQUIRE(CredentialStore::Load(kTestId, SecretKind::KeyPassphrase, b));
    REQUIRE(CredentialStore::Load(kTestId, SecretKind::ProxyPassword, c));
    REQUIRE(a.View() == "pw");
    REQUIRE(b.View() == "pass");
    REQUIRE(c.View() == "proxy");

    // Erasing one leaves the others alone.
    REQUIRE(CredentialStore::Erase(kTestId, SecretKind::Password));
    REQUIRE_FALSE(CredentialStore::Exists(kTestId, SecretKind::Password));
    REQUIRE(CredentialStore::Exists(kTestId, SecretKind::KeyPassphrase));
    REQUIRE(CredentialStore::Exists(kTestId, SecretKind::ProxyPassword));
}

TEST_CASE("overwriting replaces rather than appends", "[credstore]")
{
    Scrub guard;
    REQUIRE(CredentialStore::Store(kTestId, SecretKind::Password,
                                   SecureString("a-much-longer-original-secret")));
    REQUIRE(CredentialStore::Store(kTestId, SecretKind::Password, SecureString("short")));
    SecureString out;
    REQUIRE(CredentialStore::Load(kTestId, SecretKind::Password, out));
    REQUIRE(out.View() == "short");
    REQUIRE(out.Size() == 5);
}

TEST_CASE("erasing something absent is success, not failure", "[credstore]")
{
    Scrub guard;
    // The desired end state is "no such credential", and it already holds.
    // Reporting failure here would make every sign-out look broken.
    REQUIRE(CredentialStore::Erase(kTestId, SecretKind::Password));
    REQUIRE_FALSE(CredentialStore::Exists(kTestId, SecretKind::Password));
}

TEST_CASE("loading what is not there fails and leaves the output empty", "[credstore]")
{
    Scrub guard;
    SecureString out(std::string_view("previous contents"));
    REQUIRE_FALSE(CredentialStore::Load(kTestId, SecretKind::Password, out));
    // The old value must not survive a failed load: a caller that ignores the
    // return would otherwise authenticate with whatever was in the variable.
    REQUIRE(out.Empty());
}

TEST_CASE("an empty profile id is refused rather than sharing one slot", "[credstore]")
{
    // Every profile without an id would otherwise write to the same target
    // name and read each other's passwords.
    SecureString out;
    REQUIRE_FALSE(CredentialStore::Store("", SecretKind::Password, SecureString("x")));
    REQUIRE_FALSE(CredentialStore::Load("", SecretKind::Password, out));
    REQUIRE_FALSE(CredentialStore::Erase("", SecretKind::Password));
    REQUIRE_FALSE(CredentialStore::Exists("", SecretKind::Password));
}

TEST_CASE("target names are per profile and per kind", "[credstore]")
{
    const std::wstring pw = CredentialStore::TargetName("abc", SecretKind::Password);
    const std::wstring pp = CredentialStore::TargetName("abc", SecretKind::KeyPassphrase);
    const std::wstring other = CredentialStore::TargetName("xyz", SecretKind::Password);
    REQUIRE(pw != pp);
    REQUIRE(pw != other);
    REQUIRE(pw.rfind(L"AmberSSH/", 0) == 0);
    REQUIRE(pw.find(L"abc") != std::wstring::npos);
}
