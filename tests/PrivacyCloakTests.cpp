// PrivacyCloakTests — masking likely secrets for screen sharing.
//
// This module is the one in AmberSSH most able to do harm by being believed.
// Pattern matching cannot find every secret, so the tests below pin down three
// separate things: what it does find, what it deliberately leaves alone, and
// — the one that matters most — that nothing it produces ever claims a session
// is safe to share.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "security/PrivacyCloak.h"

using namespace amber;

namespace
{

CloakOptions On()
{
    CloakOptions o;
    o.enabled = true;
    return o;
}

std::string Mask(const std::string& line)
{
    return MaskLine(line, On());
}

bool Contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("The status text never promises safety", "[cloak]")
{
    // The roadmap's rule, verbatim: AmberSSH says "potential secrets are being
    // masked". It must NEVER say "this session is safe to share". This test is
    // the reason the wording lives in one function instead of in the UI.
    const std::string s = CloakStatusText();
    CHECK(Contains(s, "potential secrets"));
    CHECK(Contains(s, "masked"));
    CHECK_FALSE(Contains(s, "safe to share"));
    CHECK_FALSE(Contains(s, "safe"));
    CHECK_FALSE(Contains(s, "secure"));
    CHECK_FALSE(Contains(s, "all secrets"));
    // And it says out loud that it will miss things.
    CHECK(Contains(s, "does not guarantee"));
}

TEST_CASE("Disabled means untouched", "[cloak]")
{
    CloakOptions off;                     // enabled defaults to false
    const std::string line = "export AWS_SECRET_ACCESS_KEY=AKIAIOSFODNN7EXAMPLE";
    CHECK(FindSecrets(line, off).empty());
    CHECK(MaskLine(line, off) == line);
}

TEST_CASE("Well-known credential shapes are found", "[cloak]")
{
    struct Case { const char* line; const char* secret; };
    const Case kCases[] = {
        { "aws_access_key_id = AKIAIOSFODNN7EXAMPLE", "AKIAIOSFODNN7EXAMPLE" },
        { "git remote set-url origin https://ghp_16C7e42F292c6912E7710c838347Ae178B4a@github.com/x",
          "ghp_16C7e42F292c6912E7710c838347Ae178B4a" },
        { "curl -H 'x' https://hooks.slack.com xoxb-1234567890-abcdefghij",
          "xoxb-1234567890-abcdefghij" },
        { "OPENAI_KEY=sk-proj1234567890abcdefGHIJ", "sk-proj1234567890abcdefGHIJ" },
        { "token: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9",
          "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9" },
    };
    for (const Case& c : kCases)
    {
        INFO(c.line);
        const std::string masked = Mask(c.line);
        CHECK_FALSE(Contains(masked, c.secret));
        CHECK(Contains(masked, "[redacted]"));
    }
}

TEST_CASE("A secret-looking name hides its value, not itself", "[cloak]")
{
    // Hiding the NAME would make the screen unreadable and tells a viewer
    // nothing they should not already see.
    CHECK(Mask("DB_PASSWORD=hunter2correct") == "DB_PASSWORD=[redacted]");
    CHECK(Mask("api_key: \"a b c\"") == "api_key: [redacted]");
    CHECK(Mask("client_secret = 'zzz'") == "client_secret = [redacted]");
    // A name that is not a secret is left completely alone.
    CHECK(Mask("PATH=/usr/local/bin") == "PATH=/usr/local/bin");
    CHECK(Mask("count=42") == "count=42");
}

TEST_CASE("An auth header hides the token, not the scheme", "[cloak]")
{
    // Covering the word "Bearer" while leaving the token visible would be
    // worse than doing nothing: the line looks handled and is not.
    const std::string m = Mask("Authorization: Bearer abc123XYZdef456UVWghi789");
    CHECK(Contains(m, "Bearer"));
    CHECK_FALSE(Contains(m, "abc123XYZdef456UVWghi789"));
    CHECK(Contains(m, "[redacted]"));
}

TEST_CASE("Credentials in a URL and in a query string", "[cloak]")
{
    const std::string a = Mask("psql postgres://admin:hunter2@db.internal:5432/app");
    CHECK_FALSE(Contains(a, "hunter2"));
    CHECK(Contains(a, "admin"));        // the user name is context, not a secret
    CHECK(Contains(a, "db.internal"));

    const std::string b = Mask("curl 'https://api.example.com/v1/x?token=s3cr3tV4lue&page=2'");
    CHECK_FALSE(Contains(b, "s3cr3tV4lue"));
    // The rest of the query string must survive: over-masking destroys the
    // context that made sharing the screen useful.
    CHECK(Contains(b, "page=2"));
    CHECK(Contains(b, "api.example.com"));
}

TEST_CASE("Password flags on a command line", "[cloak]")
{
    CHECK_FALSE(Contains(Mask("mysql -u root --password=s3cr3tPassw0rd app"),
                         "s3cr3tPassw0rd"));
    CHECK_FALSE(Contains(Mask("app --token abcdefghijklmnop"), "abcdefghijklmnop"));
    CHECK_FALSE(Contains(Mask("mysql -p hunter2secret"), "hunter2secret"));
    // A port number after -p is not a password.
    CHECK(Mask("ssh -p 2222 host.example.com") ==
          "ssh -p 2222 host.example.com");
    // A long option that merely starts the same way is not the flag.
    CHECK(Mask("app --token-file /etc/app/tok") == "app --token-file /etc/app/tok");
}

TEST_CASE("A PEM private key is covered line by line", "[cloak]")
{
    PemState st;
    const char* kKey[] = {
        "-----BEGIN OPENSSH PRIVATE KEY-----",
        "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtz",
        "c2gtZWQyNTUxOQAAACBqZmRzYWZkc2FmZHNhZmRzYWZkc2FmZHNhZmRzYWZkc2E=",
        "-----END OPENSSH PRIVATE KEY-----",
    };
    for (const char* line : kKey)
    {
        INFO(line);
        // Either the line itself matches, or the block state says we are
        // inside a key — a caller that checks both never leaks a body line.
        const bool inKey = UpdatePem(line, st);
        CHECK(inKey);
    }
    CHECK_FALSE(st.inBlock);
    // A body line on its own, with no block state, is still caught by the
    // generic token rule rather than passing through in the clear.
    CHECK(Contains(Mask(kKey[1]), "[redacted]"));
    // The header line is masked whole.
    CHECK(Mask(kKey[0]) == "[redacted]");
}

TEST_CASE("A connection string only reaches to its own semicolon", "[cloak]")
{
    // The field ends at the semicolon. With no semicolon this is not a
    // connection string, and reading it as one covered everything to the end
    // of the line — which on a terminal row means the rest of the command.
    const std::string m = Mask("sqlcmd \"Server=db;Password=p@ss w0rd;Trusted=no\"");
    CHECK_FALSE(Contains(m, "p@ss w0rd"));
    CHECK(Contains(m, "Server=db"));
    CHECK(Contains(m, "Trusted=no"));

    const std::string n = Mask("echo DB_PASSWORD=hunter2correct and more text here");
    CHECK_FALSE(Contains(n, "hunter2correct"));
    CHECK(Contains(n, "and more text here"));
}

TEST_CASE("Ordinary output is not mangled", "[cloak]")
{
    // A masker that fires on ordinary text gets switched off, and then it
    // protects nobody. These are the lines that must survive untouched.
    const char* kClean[] = {
        "total 48",
        "drwxr-xr-x  4 josh staff   128 Sep  3 11:04 src",
        "the quick brown fox jumps over the lazy dog",
        "/usr/local/lib/python3.11/site-packages",
        "commit d41d8cd98f00b204e9800998ecf8427e5a1b2c3d",
        "550e8400-e29b-41d4-a716-446655440000",
        "Compiling amber v0.9.2 (/home/build/amber)",
        "make: *** [Makefile:42: all] Error 2",
        "HTTP/1.1 200 OK",
        "listening on port 8080",
    };
    for (const char* line : kClean)
    {
        INFO(line);
        CHECK(Mask(line) == std::string(line));
    }
}

TEST_CASE("Address and home-directory masking are opt-in", "[cloak]")
{
    const std::string line = "ssh josh@10.0.14.72 -i /home/josh/.ssh/id_ed25519";
    // Off by default: masking every address while debugging a network makes
    // the terminal useless.
    CHECK(Mask(line) == line);

    CloakOptions o = On();
    o.ipAddresses = true;
    o.homeDirectories = true;
    const std::string m = MaskLine(line, o);
    CHECK_FALSE(Contains(m, "10.0.14.72"));
    CHECK_FALSE(Contains(m, "/home/josh"));
    CHECK(Contains(m, "/home/[redacted]/.ssh/id_ed25519"));
    // A version number is not an address.
    CHECK(Contains(MaskLine("amber v0.9.2 build 14", o), "0.9.2"));
}

TEST_CASE("A user pattern is matched case-insensitively", "[cloak]")
{
    CloakOptions o = On();
    o.userLiterals.push_back("acme-corp");
    const std::string m = MaskLine("deploying to ACME-Corp production", o);
    CHECK_FALSE(Contains(m, "ACME-Corp"));
    CHECK(Contains(m, "deploying to"));
    CHECK(Contains(m, "production"));
    // An empty pattern must not match everywhere.
    o.userLiterals.push_back("");
    CHECK(MaskLine("plain line", o) == "plain line");
}

TEST_CASE("Spans are ordered, non-overlapping and inside the line", "[cloak]")
{
    const char* kLines[] = {
        "DB_PASSWORD=abc123XYZ token=def456UVW --password=ghi789RST",
        "https://u:p@h/x?token=abc123XYZdef456&password=zzz999AAA",
        "AKIAIOSFODNN7EXAMPLE and ghp_16C7e42F292c6912E7710c838347Ae178B4a",
        "",
        "=",
        "password=",
        "--token",
    };
    CloakOptions o = On();
    o.ipAddresses = true;
    o.homeDirectories = true;
    o.userLiterals.push_back("h");
    for (const char* line : kLines)
    {
        INFO(line);
        const std::string text = line;
        const std::vector<SecretSpan> v = FindSecrets(text, o);
        size_t last = 0;
        for (const SecretSpan& s : v)
        {
            CHECK(s.begin >= last);
            CHECK(s.end > s.begin);
            CHECK(s.end <= text.size());
            CHECK_FALSE(s.rule.empty());
            last = s.end;
        }
        if (v.empty())
            CHECK(ApplySpans(text, v) == text);
        else
            CHECK(Contains(ApplySpans(text, v), "[redacted]"));
    }
}

TEST_CASE("ApplySpans rebuilds the line around the covered regions", "[cloak]")
{
    const std::string line = "abcdefghij";
    std::vector<SecretSpan> v;
    SecretSpan a;
    a.begin = 2;
    a.end = 5;
    v.push_back(a);
    SecretSpan b;
    b.begin = 7;
    b.end = 9;
    v.push_back(b);
    CHECK(ApplySpans(line, v) == "ab[redacted]fg[redacted]j");
    CHECK(ApplySpans(line, {}) == line);
    // The marker is a fixed width, so the length of the secret does not leak.
    SecretSpan whole;
    whole.begin = 0;
    whole.end = line.size();
    CHECK(ApplySpans(line, { whole }) == "[redacted]");
    CHECK(ApplySpans("xy", { whole }) == "[redacted]");
}

TEST_CASE("Masking twice changes nothing further", "[cloak]")
{
    for (const char* line : { "DB_PASSWORD=hunter2correct",
                              "AKIAIOSFODNN7EXAMPLE",
                              "curl https://x/y?token=s3cr3tV4lue&page=2",
                              "nothing to see here" })
    {
        INFO(line);
        const std::string once = Mask(line);
        CHECK(Mask(once) == once);
    }
}

TEST_CASE("A redaction report counts, and never records the secret", "[cloak]")
{
    const std::string line = "DB_PASSWORD=hunter2correct API_TOKEN=abc123XYZdef";
    const std::vector<SecretSpan> v = FindSecrets(line, On());
    REQUIRE_FALSE(v.empty());
    const std::string summary = RedactionSummary(v);
    CHECK_FALSE(summary.empty());
    // The whole point of a report is that it can be kept. So it must not
    // contain what was hidden.
    CHECK_FALSE(Contains(summary, "hunter2correct"));
    CHECK_FALSE(Contains(summary, "abc123XYZdef"));
    CHECK(Contains(summary, "assignment"));
    CHECK(RedactionSummary({}) == "nothing masked");
}

TEST_CASE("A span never records the secret either", "[cloak]")
{
    const std::string line = "DB_PASSWORD=hunter2correct";
    for (const SecretSpan& s : FindSecrets(line, On()))
        CHECK_FALSE(Contains(s.rule, "hunter2correct"));
}

TEST_CASE("Every secret kind has a name", "[cloak]")
{
    for (int k = static_cast<int>(CloakKind::Token);
         k <= static_cast<int>(CloakKind::Manual); ++k)
        CHECK(std::string(CloakKindName(static_cast<CloakKind>(k))) != "unknown");
}

TEST_CASE("Turning a class off turns exactly that class off", "[cloak]")
{
    const std::string line = "psql postgres://admin:hunter2@db/app --token abcdefghijklmnop";
    CloakOptions o = On();
    o.urlCredentials = false;
    const std::string m = MaskLine(line, o);
    CHECK(Contains(m, "hunter2"));
    CHECK_FALSE(Contains(m, "abcdefghijklmnop"));
}

TEST_CASE("The token length threshold is respected", "[cloak]")
{
    CloakOptions o = On();
    o.tokens = true;
    // Short mixed tokens are far too common in ordinary output to mask.
    CHECK(MaskLine("id A1b2C3", o) == "id A1b2C3");
    o.minTokenLength = 6;
    CHECK(Contains(MaskLine("id A1b2C3", o), "[redacted]"));
}
