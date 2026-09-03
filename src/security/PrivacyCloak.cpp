#include "PrivacyCloak.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace amber
{

namespace
{

// The body of an opaque credential. '=' is NOT here: it is base64 padding and
// only ever appears at the end, so treating it as an interior character would
// glue "NAME=value" into one enormous token and mask the name along with it.
bool IsB64(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' ||
           c == '-' || c == '_';
}

// Advances past up to two '=' of base64 padding at the end of a token.
size_t EatPadding(const std::string& s, size_t e)
{
    for (int k = 0; k < 2 && e < s.size() && s[e] == '='; ++k)
        ++e;
    return e;
}

bool IsWordChar(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A name that suggests its value is a secret. Substring match, so
// "AWS_SECRET_ACCESS_KEY" and "db_password" both hit.
bool SecretName(const std::string& name)
{
    static const char* kWords[] = {
        "password", "passwd", "secret", "token", "apikey", "api_key",
        "access_key", "accesskey", "private_key", "privatekey", "credential",
        "auth", "bearer", "session_id", "sessionid", "cookie", "passphrase",
        "client_secret", "signing_key", "encryption_key",
    };
    const std::string low = Lower(name);
    for (const char* w : kWords)
        if (low.find(w) != std::string::npos)
            return true;
    return false;
}

void Push(std::vector<SecretSpan>& out, size_t b, size_t e, CloakKind k,
          const char* rule)
{
    if (e <= b)
        return;
    SecretSpan s;
    s.begin = b;
    s.end = e;
    s.kind = k;
    s.rule = rule;
    out.push_back(s);
}

// Merges overlapping spans, keeping the first rule that matched — the report
// then names one reason per covered region rather than several for the same
// bytes.
void Normalise(std::vector<SecretSpan>& v)
{
    if (v.empty())
        return;
    std::stable_sort(v.begin(), v.end(),
                     [](const SecretSpan& a, const SecretSpan& b)
                     { return a.begin < b.begin; });
    std::vector<SecretSpan> out;
    for (const SecretSpan& s : v)
    {
        if (!out.empty() && s.begin <= out.back().end)
        {
            out.back().end = std::max(out.back().end, s.end);
            continue;
        }
        out.push_back(s);
    }
    v.swap(out);
}

// --- individual detectors ------------------------------------------------

// Well-known credential shapes, matched on their prefix so the whole token is
// covered even when it contains characters a generic scan would stop at.
void FindKnownTokens(const std::string& s, std::vector<SecretSpan>& out)
{
    struct Pat { const char* prefix; const char* rule; };
    static const Pat kPats[] = {
        { "AKIA",      "AWS access key id" },
        { "ASIA",      "AWS temporary key id" },
        { "ghp_",      "GitHub personal access token" },
        { "gho_",      "GitHub OAuth token" },
        { "ghu_",      "GitHub user token" },
        { "ghs_",      "GitHub server token" },
        { "ghr_",      "GitHub refresh token" },
        { "github_pat_", "GitHub fine-grained token" },
        { "xoxb-",     "Slack bot token" },
        { "xoxp-",     "Slack user token" },
        { "xapp-",     "Slack app token" },
        { "sk-",       "OpenAI-style secret key" },
        { "sk_live_",  "Stripe live secret key" },
        { "sk_test_",  "Stripe test secret key" },
        { "rk_live_",  "Stripe restricted key" },
        { "AIza",      "Google API key" },
        { "ya29.",     "Google OAuth token" },
        { "glpat-",    "GitLab personal access token" },
        { "npm_",      "npm token" },
        { "dop_v1_",   "DigitalOcean token" },
        { "SG.",       "SendGrid key" },
        { "eyJ",       "JWT" },
    };
    for (const Pat& p : kPats)
    {
        const size_t plen = std::char_traits<char>::length(p.prefix);
        size_t at = 0;
        while ((at = s.find(p.prefix, at)) != std::string::npos)
        {
            // Must start a token, not sit inside a longer word.
            if (at > 0 && IsWordChar(s[at - 1]))
            {
                at += plen;
                continue;
            }
            size_t e = at + plen;
            while (e < s.size() && (IsB64(s[e]) || s[e] == '.'))
                ++e;
            e = EatPadding(s, e);
            // A prefix with nothing after it is not a credential.
            if (e - at >= plen + 8)
                Push(out, at, e, CloakKind::Token, p.rule);
            at = e;
        }
    }
}

// NAME=value and NAME: value where NAME looks like a secret. Only the VALUE
// is covered: hiding the name too would make the output unreadable and tells
// the viewer nothing they should not see.
void FindAssignments(const std::string& s, std::vector<SecretSpan>& out)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '=' && s[i] != ':')
            continue;
        // The name immediately to the left.
        size_t nameEnd = i;
        while (nameEnd > 0 && isspace(static_cast<unsigned char>(s[nameEnd - 1])))
            --nameEnd;
        size_t nameBegin = nameEnd;
        while (nameBegin > 0 && IsWordChar(s[nameBegin - 1]))
            --nameBegin;
        if (nameBegin == nameEnd)
            continue;
        if (!SecretName(s.substr(nameBegin, nameEnd - nameBegin)))
            continue;
        // The value to the right, respecting quotes.
        size_t v = i + 1;
        while (v < s.size() && isspace(static_cast<unsigned char>(s[v])))
            ++v;
        if (v >= s.size())
            continue;
        // "Authorization: Bearer <token>" — the scheme word is not the secret,
        // and covering it while leaving the token visible would be worse than
        // covering nothing, because it looks like the line was handled.
        static const char* kSchemes[] = { "bearer", "basic", "token", "digest" };
        for (const char* scheme : kSchemes)
        {
            const size_t slen = std::char_traits<char>::length(scheme);
            if (Lower(s.substr(v, slen)) != scheme)
                continue;
            const size_t after = v + slen;
            if (after < s.size() && isspace(static_cast<unsigned char>(s[after])))
            {
                v = after;
                while (v < s.size() && isspace(static_cast<unsigned char>(s[v])))
                    ++v;
            }
            break;
        }
        if (v >= s.size())
            continue;
        size_t e = v;
        if (s[v] == '"' || s[v] == '\'')
        {
            const char q = s[v];
            e = v + 1;
            while (e < s.size() && s[e] != q)
                ++e;
            if (e < s.size())
                ++e;
        }
        else
        {
            // Stop where a shell or a URL would end the word. Running past an
            // '&' would swallow the rest of a query string and hide fields
            // that are not secrets.
            while (e < s.size() && !isspace(static_cast<unsigned char>(s[e])) &&
                   s[e] != ';' && s[e] != ',' && s[e] != '&' && s[e] != '|' &&
                   s[e] != ')')
                ++e;
        }
        Push(out, v, e, CloakKind::Assignment, "secret-looking name");
        i = e;
    }
}

// --password x, --password=x, -p x, and the same for token/secret/key.
void FindCliArgs(const std::string& s, std::vector<SecretSpan>& out)
{
    static const char* kFlags[] = {
        "--password", "--passwd", "--token", "--secret", "--api-key",
        "--apikey", "--auth", "--bearer", "--passphrase", "--client-secret",
        "--access-key", "--private-key",
    };
    for (const char* f : kFlags)
    {
        const size_t flen = std::char_traits<char>::length(f);
        size_t at = 0;
        while ((at = s.find(f, at)) != std::string::npos)
        {
            size_t after = at + flen;
            // Must be the whole flag: "--token" not "--token-file".
            if (after < s.size() && IsWordChar(s[after]) && s[after] != '=')
            {
                at = after;
                continue;
            }
            size_t v = after;
            if (v < s.size() && s[v] == '=')
                ++v;
            else
                while (v < s.size() && isspace(static_cast<unsigned char>(s[v])))
                    ++v;
            if (v >= s.size())
                break;
            size_t e = v;
            if (s[v] == '"' || s[v] == '\'')
            {
                const char q = s[v];
                e = v + 1;
                while (e < s.size() && s[e] != q)
                    ++e;
                if (e < s.size())
                    ++e;
            }
            else
                while (e < s.size() && !isspace(static_cast<unsigned char>(s[e])))
                    ++e;
            Push(out, v, e, CloakKind::CliArgument, f);
            at = e;
        }
    }
    // The short forms, which only count with a space or an attached value.
    size_t at = 0;
    while ((at = s.find("-p", at)) != std::string::npos)
    {
        const bool startsToken = at == 0 || isspace(static_cast<unsigned char>(s[at - 1]));
        if (!startsToken || at + 2 >= s.size())
        {
            at += 2;
            continue;
        }
        size_t v = at + 2;
        if (isspace(static_cast<unsigned char>(s[v])))
            while (v < s.size() && isspace(static_cast<unsigned char>(s[v])))
                ++v;
        size_t e = v;
        while (e < s.size() && !isspace(static_cast<unsigned char>(s[e])))
            ++e;
        // Only when the value looks like a credential rather than a number or
        // a path — "-p 8080" and "-p ./dir" are not passwords.
        const std::string val = s.substr(v, e - v);
        const bool numeric = !val.empty() &&
                             val.find_first_not_of("0123456789") == std::string::npos;
        if (!val.empty() && !numeric && val.find('/') == std::string::npos &&
            val.size() >= 6)
            Push(out, v, e, CloakKind::CliArgument, "-p");
        at = e;
    }
}

// user:pass@host, and tokens in a query string.
void FindUrlCredentials(const std::string& s, std::vector<SecretSpan>& out)
{
    size_t at = 0;
    while ((at = s.find("://", at)) != std::string::npos)
    {
        const size_t authBegin = at + 3;
        size_t e = authBegin;
        while (e < s.size() && !isspace(static_cast<unsigned char>(s[e])) &&
               s[e] != '/' && s[e] != '"' && s[e] != '\'')
            ++e;
        const size_t atSign = s.find('@', authBegin);
        if (atSign != std::string::npos && atSign < e)
        {
            const size_t colon = s.find(':', authBegin);
            if (colon != std::string::npos && colon < atSign)
                Push(out, colon + 1, atSign, CloakKind::UrlCredential,
                     "password in a URL");
        }
        at = e;
    }
    // ?token=... &access_token=... &key=...
    static const char* kParams[] = { "token=", "access_token=", "api_key=",
                                     "apikey=", "key=", "secret=", "password=",
                                     "auth=", "sig=", "signature=" };
    for (const char* p : kParams)
    {
        const size_t plen = std::char_traits<char>::length(p);
        size_t k = 0;
        while ((k = s.find(p, k)) != std::string::npos)
        {
            const bool boundary = k == 0 || s[k - 1] == '?' || s[k - 1] == '&';
            if (!boundary)
            {
                k += plen;
                continue;
            }
            size_t v = k + plen;
            size_t e = v;
            while (e < s.size() && s[e] != '&' && s[e] != '#' &&
                   !isspace(static_cast<unsigned char>(s[e])) && s[e] != '"' &&
                   s[e] != '\'')
                ++e;
            Push(out, v, e, CloakKind::UrlCredential, "token in a query string");
            k = e;
        }
    }
}

void FindConnectionStrings(const std::string& s, std::vector<SecretSpan>& out)
{
    // "Password=x;" inside an ODBC/ADO connection string. The assignment
    // detector catches most of these; this one exists for the semicolon form
    // where the value may contain spaces.
    // Only the credential fields. A user id is not a secret, and masking it
    // costs the reader context for nothing.
    static const char* kKeys[] = { "password=", "pwd=" };
    const std::string low = Lower(s);
    for (const char* k : kKeys)
    {
        const size_t klen = std::char_traits<char>::length(k);
        size_t at = 0;
        while ((at = low.find(k, at)) != std::string::npos)
        {
            const size_t v = at + klen;
            // A connection-string field ends at its semicolon. With no
            // semicolon this is not a connection string at all — it is an
            // ordinary NAME=value, which the assignment rule already handles
            // and stops at the end of the WORD. Running to the end of the line
            // here covered the rest of the command, which is over-masking of
            // the kind that gets the whole feature switched off.
            const size_t semi = s.find(';', v);
            if (semi == std::string::npos)
            {
                at = v;
                continue;
            }
            Push(out, v, semi, CloakKind::ConnectionString, "connection string");
            at = semi;
        }
    }
}

void FindGenericTokens(const std::string& s, std::vector<SecretSpan>& out,
                       size_t minLen)
{
    // A long unbroken run of base64-ish characters that contains BOTH letters
    // and digits. Requiring both is what keeps this off English words, long
    // paths and hex-only values like commit hashes, which are not secrets.
    size_t i = 0;
    while (i < s.size())
    {
        if (!IsB64(s[i]))
        {
            ++i;
            continue;
        }
        size_t e = i;
        while (e < s.size() && IsB64(s[e]))
            ++e;
        const size_t len = e - i;
        e = EatPadding(s, e);
        if (len >= minLen)
        {
            bool hasAlpha = false, hasDigit = false, hasUpper = false;
            for (size_t k = i; k < e; ++k)
            {
                const unsigned char c = static_cast<unsigned char>(s[k]);
                if (isdigit(c)) hasDigit = true;
                else if (isalpha(c)) { hasAlpha = true; if (isupper(c)) hasUpper = true; }
            }
            // Mixed case AND digits: the signature of a generated credential
            // rather than a sentence, an identifier or a hex digest.
            if (hasAlpha && hasDigit && hasUpper)
                Push(out, i, e, CloakKind::Token, "long mixed-case token");
        }
        i = e;
    }
}

void FindIpAddresses(const std::string& s, std::vector<SecretSpan>& out)
{
    size_t i = 0;
    while (i < s.size())
    {
        if (!isdigit(static_cast<unsigned char>(s[i])))
        {
            ++i;
            continue;
        }
        const size_t start = i;
        int parts = 0;
        size_t e = i;
        while (e < s.size() && parts < 4)
        {
            size_t d = e;
            while (d < s.size() && isdigit(static_cast<unsigned char>(s[d])))
                ++d;
            if (d == e)
                break;
            ++parts;
            e = d;
            if (parts < 4 && e < s.size() && s[e] == '.')
                ++e;
            else
                break;
        }
        if (parts == 4 && (start == 0 || !IsWordChar(s[start - 1])) &&
            (e >= s.size() || !IsWordChar(s[e])))
            Push(out, start, e, CloakKind::IpAddress, "IPv4 address");
        i = e > i ? e : i + 1;
    }
}

void FindHomeDirs(const std::string& s, std::vector<SecretSpan>& out)
{
    // /home/<name> and C:\Users\<name>: the NAME only, so the path stays
    // readable and only the identity is covered.
    struct Pat { const char* prefix; };
    static const Pat kPats[] = { { "/home/" }, { "/Users/" }, { "\\Users\\" } };
    for (const Pat& p : kPats)
    {
        const size_t plen = std::char_traits<char>::length(p.prefix);
        size_t at = 0;
        while ((at = s.find(p.prefix, at)) != std::string::npos)
        {
            const size_t v = at + plen;
            size_t e = v;
            while (e < s.size() && s[e] != '/' && s[e] != '\\' &&
                   !isspace(static_cast<unsigned char>(s[e])))
                ++e;
            Push(out, v, e, CloakKind::HomeDirectory, "home directory name");
            at = e;
        }
    }
}

void FindLiterals(const std::string& s, const std::vector<std::string>& lits,
                  std::vector<SecretSpan>& out)
{
    const std::string low = Lower(s);
    for (const std::string& lit : lits)
    {
        if (lit.empty())
            continue;
        const std::string l = Lower(lit);
        size_t at = 0;
        while ((at = low.find(l, at)) != std::string::npos)
        {
            Push(out, at, at + l.size(), CloakKind::UserDefined, "user pattern");
            at += l.size();
        }
    }
}

} // namespace

const char* CloakKindName(CloakKind k)
{
    switch (k)
    {
    case CloakKind::Token:            return "token";
    case CloakKind::Assignment:       return "assignment";
    case CloakKind::CliArgument:      return "command argument";
    case CloakKind::UrlCredential:    return "URL credential";
    case CloakKind::PrivateKey:       return "private key";
    case CloakKind::ConnectionString: return "connection string";
    case CloakKind::IpAddress:        return "IP address";
    case CloakKind::Hostname:         return "host name";
    case CloakKind::HomeDirectory:    return "home directory";
    case CloakKind::UserDefined:      return "user pattern";
    case CloakKind::Manual:           return "manual redaction";
    }
    return "unknown";
}

bool UpdatePem(const std::string& line, PemState& st)
{
    if (line.find("-----BEGIN") != std::string::npos &&
        line.find("PRIVATE KEY-----") != std::string::npos)
    {
        st.inBlock = true;
        return true;
    }
    if (st.inBlock)
    {
        if (line.find("-----END") != std::string::npos)
            st.inBlock = false;
        return true;
    }
    return false;
}

std::vector<SecretSpan> FindSecrets(const std::string& line, const CloakOptions& o)
{
    std::vector<SecretSpan> out;
    if (!o.enabled || line.empty())
        return out;

    // A PEM header on its own line covers the whole line; the body is covered
    // by the caller's PemState, because a key spans many lines.
    if (o.privateKeys && line.find("PRIVATE KEY-----") != std::string::npos)
    {
        Push(out, 0, line.size(), CloakKind::PrivateKey, "PEM private key");
        return out;
    }

    if (o.tokens)
        FindKnownTokens(line, out);
    if (o.assignments)
        FindAssignments(line, out);
    if (o.cliArguments)
        FindCliArgs(line, out);
    if (o.urlCredentials)
        FindUrlCredentials(line, out);
    if (o.connectionStrings)
        FindConnectionStrings(line, out);
    if (o.tokens)
        FindGenericTokens(line, out, o.minTokenLength);
    if (o.ipAddresses)
        FindIpAddresses(line, out);
    if (o.homeDirectories)
        FindHomeDirs(line, out);
    if (!o.userLiterals.empty())
        FindLiterals(line, o.userLiterals, out);

    Normalise(out);
    return out;
}

std::string ApplySpans(const std::string& line, const std::vector<SecretSpan>& spans)
{
    if (spans.empty())
        return line;
    std::string out;
    size_t at = 0;
    for (const SecretSpan& s : spans)
    {
        if (s.begin > line.size())
            break;
        const size_t e = std::min(s.end, line.size());
        if (s.begin > at)
            out.append(line, at, s.begin - at);
        // A fixed marker rather than one block per character: a run of blocks
        // leaks the length of the secret, which for a password is a real hint.
        out += "[redacted]";
        at = e;
    }
    if (at < line.size())
        out.append(line, at, line.size() - at);
    return out;
}

std::string MaskLine(const std::string& line, const CloakOptions& o)
{
    return ApplySpans(line, FindSecrets(line, o));
}

const char* CloakStatusText()
{
    // The one place this claim is made, and it is deliberately the weaker,
    // true one. Detection is pattern matching; it will miss things.
    return "Privacy Cloak on — potential secrets are being masked. "
           "This does not guarantee every secret is hidden.";
}

std::string RedactionSummary(const std::vector<SecretSpan>& spans)
{
    if (spans.empty())
        return "nothing masked";
    std::map<std::string, int> byKind;
    for (const SecretSpan& s : spans)
        ++byKind[CloakKindName(s.kind)];
    std::string out;
    for (const auto& [kind, count] : byKind)
    {
        if (!out.empty())
            out += ", ";
        out += std::to_string(count) + " " + kind;
        if (count != 1)
            out += "s";
    }
    // Deliberately no values, no offsets into a stored line, nothing that
    // could reconstruct what was hidden.
    return out;
}

} // namespace amber
