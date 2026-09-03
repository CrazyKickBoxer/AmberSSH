// SyntaxTint.h — visual-only tinting of the *current input line*.
//
// Hard rules, enforced by the design and by tests:
//   * never modifies bytes sent to the remote shell
//   * never injects escape sequences into the SSH stream
//   * suppressed on the alternate screen, at password prompts, and whenever
//     confidence is low
//
// This is a deliberately conservative heuristic tokenizer, not a shell parser.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace amber
{

enum class TokenKind : uint8_t
{
    None = 0,
    Command,     // first word on the line
    Flag,        // -x, --long, /switch
    Path,        // /abs, ./rel, ../rel, ~/home, C:\win
    String,      // "quoted" or 'quoted'
    Operator,    // | || && ; > >> < 2>&1
};

struct TintSpan
{
    int start = 0;          // byte offset into the analysed line
    int length = 0;
    TokenKind kind = TokenKind::None;
};

// Suggested colours (0xRRGGBB), matching the specification.
inline uint32_t TintColor(TokenKind k)
{
    switch (k)
    {
    case TokenKind::Command:  return 0x35D9FF;
    case TokenKind::Flag:     return 0xFF8A30;
    case TokenKind::Path:     return 0x65E572;
    case TokenKind::String:   return 0xFF6FB5;
    case TokenKind::Operator: return 0xFFD166;
    default:                  return 0;
    }
}

class SyntaxTint
{
public:
    // Tokenises one command line. `line` is the text after the shell prompt.
    static std::vector<TintSpan> Analyze(std::string_view line);

    // Heuristic: does this look like a prompt asking for a secret? Tinting and
    // echo-sensitive behaviour must back off when it does.
    static bool LooksLikePasswordPrompt(std::string_view text);

    // Finds where the prompt ends and user input begins. Returns 0 when no
    // recognisable prompt is present.
    static size_t FindPromptEnd(std::string_view row);
};

} // namespace amber
