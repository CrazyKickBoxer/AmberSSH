#include "SyntaxTint.h"

#include <algorithm>
#include <cctype>

namespace amber
{
namespace
{

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

bool IsOperatorStart(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '>' || c == '<';
}

// A token is a path if it carries a separator in a position that implies one,
// or uses an explicit relative/home prefix, or looks like a drive letter.
bool LooksLikePath(std::string_view t)
{
    if (t.empty())
        return false;
    if (t[0] == '/' || t[0] == '~')
        return true;
    if (t.size() >= 2 && t[0] == '.' && (t[1] == '/' || t[1] == '.'))
        return true;
    if (t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) &&
        t[1] == ':' && (t[2] == '\\' || t[2] == '/'))
        return true;
    // A bare separator anywhere else still reads as a path (dir/file).
    return t.find('/') != std::string_view::npos && t.find("://") == std::string_view::npos;
}

bool LooksLikeFlag(std::string_view t)
{
    if (t.size() < 2)
        return false;
    if (t[0] == '-')
        return true;
    // Windows-style switch, but only when it is short and not a path.
    if (t[0] == '/' && t.size() <= 4 &&
        std::all_of(t.begin() + 1, t.end(),
                    [](char c) { return std::isalnum(static_cast<unsigned char>(c)); }))
        return true;
    return false;
}

} // namespace

std::vector<TintSpan> SyntaxTint::Analyze(std::string_view line)
{
    std::vector<TintSpan> spans;
    if (line.empty() || line.size() > 4096)
        return spans;               // absurdly long lines: do not tint

    size_t i = 0;
    bool expectCommand = true;      // first token, and after every separator

    while (i < line.size())
    {
        if (IsSpace(line[i]))
        {
            ++i;
            continue;
        }

        // --- quoted string ---------------------------------------------
        if (line[i] == '"' || line[i] == '\'')
        {
            char quote = line[i];
            size_t start = i++;
            while (i < line.size() && line[i] != quote)
            {
                if (line[i] == '\\' && i + 1 < line.size())
                    ++i;            // skip the escaped character
                ++i;
            }
            if (i < line.size())
                ++i;                // closing quote
            spans.push_back({ (int)start, (int)(i - start), TokenKind::String });
            expectCommand = false;
            continue;
        }

        // --- operator ---------------------------------------------------
        if (IsOperatorStart(line[i]))
        {
            size_t start = i;
            // Redirections may carry a leading fd digit, e.g. 2>&1.
            while (i < line.size() &&
                   (IsOperatorStart(line[i]) || line[i] == '&' ||
                    std::isdigit(static_cast<unsigned char>(line[i]))))
            {
                ++i;
                if (i - start > 4)
                    break;
            }
            spans.push_back({ (int)start, (int)(i - start), TokenKind::Operator });
            expectCommand = true;   // a new command follows a separator
            continue;
        }

        // --- bare word ---------------------------------------------------
        size_t start = i;
        while (i < line.size() && !IsSpace(line[i]) && !IsOperatorStart(line[i]) &&
               line[i] != '"' && line[i] != '\'')
            ++i;
        std::string_view token = line.substr(start, i - start);
        if (token.empty())
            continue;

        TokenKind kind;
        if (expectCommand && !LooksLikeFlag(token))
        {
            // A leading token that is plainly a path is still a command
            // (./configure), so Command wins here by design.
            kind = TokenKind::Command;
            expectCommand = false;
        }
        else if (LooksLikeFlag(token))
        {
            kind = TokenKind::Flag;
        }
        else if (LooksLikePath(token))
        {
            kind = TokenKind::Path;
        }
        else
        {
            kind = TokenKind::None;
        }

        if (kind != TokenKind::None)
            spans.push_back({ (int)start, (int)token.size(), kind });
    }

    return spans;
}

bool SyntaxTint::LooksLikePasswordPrompt(std::string_view text)
{
    // Compare case-insensitively against the endings real prompts use.
    std::string lower;
    lower.reserve(text.size());
    for (char c : text)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    static const char* kNeedles[] = {
        "password:", "password for", "passphrase", "pin:",
        "verification code", "otp:", "authenticator",
    };
    for (const char* needle : kNeedles)
        if (lower.find(needle) != std::string::npos)
            return true;
    return false;
}

size_t SyntaxTint::FindPromptEnd(std::string_view row)
{
    // Recognise only the common, unambiguous prompt terminators, and require a
    // following space so that a URL or a path cannot be mistaken for a prompt.
    static const char* kEnders[] = { "$ ", "# ", "> ", "% " };
    size_t best = 0;
    for (const char* ender : kEnders)
    {
        size_t pos = row.rfind(ender);
        if (pos != std::string_view::npos)
            best = std::max(best, pos + 2);
    }
    return best;
}

} // namespace amber
