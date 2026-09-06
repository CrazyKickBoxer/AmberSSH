#include "HostKeyDecision.h"

#include <cctype>

namespace amber
{
namespace
{

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string StripPrefix(std::string s)
{
    if (s.size() >= 7)
    {
        bool sha = true;
        static const char* p = "sha256:";
        for (size_t i = 0; i < 7; ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != p[i])
            {
                sha = false;
                break;
            }
        if (sha)
            return s.substr(7);
    }
    return s;
}

bool EqualsNoCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

} // namespace

bool HostKeyFingerprintMatches(const std::string& entry, const std::string& fingerprint)
{
    const std::string e = StripPrefix(Trim(entry));
    const std::string f = StripPrefix(Trim(fingerprint));
    // An empty fingerprint must not be matchable, or a server whose key could
    // not be hashed would be accepted by an empty or whitespace-only entry.
    if (e.empty() || f.empty())
        return false;
    return EqualsNoCase(e, f);
}

HostKeyVerdict DecideHostKey(KnownHostResult lookup,
                             const std::string& fingerprint,
                             const std::vector<std::string>& manualKeys)
{
    HostKeyVerdict v;

    if (!manualKeys.empty())
    {
        for (const std::string& k : manualKeys)
            if (HostKeyFingerprintMatches(k, fingerprint))
            {
                v.action = HostKeyAction::Accept;
                v.persist = false;   // configuration, not a decision to remember
                return v;
            }
        v.action = HostKeyAction::RefuseNotListed;
        return v;
    }

    switch (lookup)
    {
    case KnownHostResult::Match:
        v.action = HostKeyAction::Accept;
        return v;
    case KnownHostResult::Mismatch:
        v.action = HostKeyAction::RefuseMismatch;
        return v;
    case KnownHostResult::NotFound:
    case KnownHostResult::Failure:
    default:
        // A lookup that could not run is treated as "unknown", not as "fine".
        // It asks; it never proceeds silently.
        v.action = HostKeyAction::Prompt;
        v.persist = true;
        return v;
    }
}

} // namespace amber
