// SyntaxTintTests.cpp — the input-line tint heuristic, including the cases
// where it must refuse to tint.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "term/SyntaxTint.h"

using namespace amber;

namespace
{

TokenKind KindAt(const std::vector<TintSpan>& spans, std::string_view line,
                 std::string_view token)
{
    size_t pos = line.find(token);
    if (pos == std::string_view::npos)
        return TokenKind::None;
    for (const auto& s : spans)
        if ((size_t)s.start == pos && (size_t)s.length == token.size())
            return s.kind;
    return TokenKind::None;
}

} // namespace

TEST_CASE("the first token is the command", "[tint]")
{
    std::string line = "ls -la /var/log";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "ls") == TokenKind::Command);
}

TEST_CASE("short and long flags are tinted as flags", "[tint]")
{
    std::string line = "grep -i --color=auto pattern";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "-i") == TokenKind::Flag);
    REQUIRE(KindAt(spans, line, "--color=auto") == TokenKind::Flag);
}

TEST_CASE("absolute, relative and home paths are tinted as paths", "[tint]")
{
    std::string line = "cp /etc/hosts ./backup ~/keep dir/file";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "/etc/hosts") == TokenKind::Path);
    REQUIRE(KindAt(spans, line, "./backup") == TokenKind::Path);
    REQUIRE(KindAt(spans, line, "~/keep") == TokenKind::Path);
    REQUIRE(KindAt(spans, line, "dir/file") == TokenKind::Path);
}

TEST_CASE("a windows drive path is a path", "[tint]")
{
    std::string line = R"(type C:\Windows\win.ini)";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, R"(C:\Windows\win.ini)") == TokenKind::Path);
}

TEST_CASE("quoted strings are tinted, including embedded spaces", "[tint]")
{
    std::string line = "echo \"hello world\" 'single quoted'";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "\"hello world\"") == TokenKind::String);
    REQUIRE(KindAt(spans, line, "'single quoted'") == TokenKind::String);
}

TEST_CASE("an escaped quote does not end the string early", "[tint]")
{
    std::string line = R"(echo "a \" b" tail)";
    auto spans = SyntaxTint::Analyze(line);
    // The span must swallow the escaped quote and stop at the real closing one.
    const TintSpan* str = nullptr;
    for (const auto& s : spans)
        if (s.kind == TokenKind::String)
            str = &s;
    REQUIRE(str != nullptr);
    std::string covered = line.substr(str->start, str->length);
    REQUIRE(covered == "\"a \\\" b\"");
    REQUIRE(line.substr(str->start + str->length).find("tail") != std::string::npos);
}

TEST_CASE("pipes and redirections are operators", "[tint]")
{
    std::string line = "cat a | sort > out";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "|") == TokenKind::Operator);
    REQUIRE(KindAt(spans, line, ">") == TokenKind::Operator);
}

TEST_CASE("the token after a pipe is a new command", "[tint]")
{
    std::string line = "cat file | grep x";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "cat") == TokenKind::Command);
    REQUIRE(KindAt(spans, line, "grep") == TokenKind::Command);
}

TEST_CASE("a leading relative path is still the command", "[tint]")
{
    std::string line = "./configure --prefix=/usr";
    auto spans = SyntaxTint::Analyze(line);
    REQUIRE(KindAt(spans, line, "./configure") == TokenKind::Command);
    REQUIRE(KindAt(spans, line, "--prefix=/usr") == TokenKind::Flag);
}

TEST_CASE("an empty line yields no spans", "[tint]")
{
    REQUIRE(SyntaxTint::Analyze("").empty());
    REQUIRE(SyntaxTint::Analyze("    ").empty());
}

TEST_CASE("an absurdly long line is not tinted", "[tint][bounds]")
{
    std::string huge(9000, 'a');
    REQUIRE(SyntaxTint::Analyze(huge).empty());
}

TEST_CASE("spans never exceed the line bounds", "[tint][bounds]")
{
    std::string line = "cmd \"unterminated";
    auto spans = SyntaxTint::Analyze(line);
    for (const auto& s : spans)
    {
        REQUIRE(s.start >= 0);
        REQUIRE(s.length >= 0);
        REQUIRE((size_t)(s.start + s.length) <= line.size());
    }
}

TEST_CASE("password prompts are recognised so tinting can back off", "[tint][security]")
{
    REQUIRE(SyntaxTint::LooksLikePasswordPrompt("Password:"));
    REQUIRE(SyntaxTint::LooksLikePasswordPrompt("[sudo] password for jo:"));
    REQUIRE(SyntaxTint::LooksLikePasswordPrompt("Enter passphrase for key"));
    REQUIRE(SyntaxTint::LooksLikePasswordPrompt("Verification code:"));
    REQUIRE_FALSE(SyntaxTint::LooksLikePasswordPrompt("user@host:~$ "));
    REQUIRE_FALSE(SyntaxTint::LooksLikePasswordPrompt("ls -la"));
}

TEST_CASE("prompt end is found for common shells", "[tint]")
{
    REQUIRE(SyntaxTint::FindPromptEnd("user@host:~$ ls") == 13);
    REQUIRE(SyntaxTint::FindPromptEnd("root@box:/# whoami") == 12);
    REQUIRE(SyntaxTint::FindPromptEnd("no prompt here") == 0);
}

TEST_CASE("the tint palette matches the specified colours", "[tint]")
{
    REQUIRE(TintColor(TokenKind::Command)  == 0x35D9FFu);
    REQUIRE(TintColor(TokenKind::Flag)     == 0xFF8A30u);
    REQUIRE(TintColor(TokenKind::Path)     == 0x65E572u);
    REQUIRE(TintColor(TokenKind::String)   == 0xFF6FB5u);
    REQUIRE(TintColor(TokenKind::Operator) == 0xFFD166u);
    REQUIRE(TintColor(TokenKind::None)     == 0u);
}
