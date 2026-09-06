// PrivacyCloak.h — deterministic masking of likely secrets, for screen
// sharing and recording.
//
// The honesty rule from the roadmap is load-bearing and this header states it
// where nobody can miss it:
//
//     AmberSSH says "potential secrets are being masked".
//     AmberSSH must NEVER say "this session is safe to share".
//
// Detection is pattern matching. It will miss things. Every function here is
// named and documented so that a caller cannot accidentally present its
// output as a guarantee.
//
// Pure: no Windows, no renderer, no terminal. It takes a line of text and
// returns the spans to cover, so the grid itself is never modified — masking
// is a presentation decision applied at draw time, exactly like every other
// overlay in this application.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

// What a span was matched by, so the UI can explain a mask and the user can
// exclude a whole class of false positive without turning everything off.
enum class CloakKind
{
    Token,          // a long opaque credential: API keys, bearer tokens
    Assignment,     // NAME=value where NAME looks like a secret
    CliArgument,    // --password=x, -p x
    UrlCredential,  // user:pass@host, or a token in a query string
    PrivateKey,     // PEM block
    ConnectionString,
    IpAddress,
    Hostname,
    HomeDirectory,
    UserDefined,    // a pattern the user added
    Manual,         // a region the user redacted by hand
};

const char* CloakKindName(CloakKind k);

// A half-open [begin, end) range of BYTE offsets in the line it came from.
struct SecretSpan
{
    size_t begin = 0, end = 0;
    CloakKind kind = CloakKind::Token;
    // Which rule matched, for the redaction report. Never the value itself:
    // a report that records what was hidden must not record the secret.
    std::string rule;
};

struct CloakOptions
{
    bool enabled = false;
    // Each class can be turned off on its own, because the cost of a false
    // positive differs wildly: masking an IP address in a demo is helpful,
    // masking every IP while debugging a network is not.
    bool tokens = true;
    bool assignments = true;
    bool cliArguments = true;
    bool urlCredentials = true;
    bool privateKeys = true;
    bool connectionStrings = true;
    bool ipAddresses = false;
    bool homeDirectories = false;
    // Extra literal substrings the user wants hidden — a host name, a
    // customer name, their own username. Matched case-insensitively.
    std::vector<std::string> userLiterals;
    // The shortest run that can be a token. Below about 16 characters the
    // false-positive rate makes the feature unusable.
    size_t minTokenLength = 20;
};

// Every span of `line` that should be covered. Sorted by begin, non
// overlapping, and never empty-length. `line` is not modified — nothing in
// this module can modify anything.
std::vector<SecretSpan> FindSecrets(const std::string& line, const CloakOptions& o);

// Applies the spans, replacing each with a fixed-width marker. Used for the
// journal, notifications, recordings and exports — the places where the text
// is COPIED somewhere rather than drawn.
std::string MaskLine(const std::string& line, const CloakOptions& o);
std::string ApplySpans(const std::string& line, const std::vector<SecretSpan>& spans);

// The same, for a line that still has its terminal escape sequences in it.
//
// A session recording is a byte stream that has to replay, so the escapes
// cannot be stripped the way a plain-text log strips them, and they must not
// be masked either: a detector matching across "\x1b[32m" would cut a colour
// change in half and corrupt everything after it. This masks only the
// printable runs between escape sequences and passes the sequences through
// untouched.
//
// The consequence, stated because it matters: a secret split across an escape
// sequence is two shorter runs and may not match a detector that would have
// matched the whole. Masking a recording is therefore weaker than masking a
// log, and neither is a guarantee.
std::string MaskTerminalLine(const std::string& line, const CloakOptions& o);

// True when the line begins or continues a PEM private-key block. A key is
// many lines, so a caller masking a stream has to carry this state.
struct PemState
{
    bool inBlock = false;
};
bool UpdatePem(const std::string& line, PemState& st);

// The wording the UI must use. Provided as a function so there is exactly one
// place the claim is made, and it is the honest one.
const char* CloakStatusText();
// A one-line summary of what was hidden, for a redaction report. Counts by
// kind; never contains a secret value.
std::string RedactionSummary(const std::vector<SecretSpan>& spans);

} // namespace amber
