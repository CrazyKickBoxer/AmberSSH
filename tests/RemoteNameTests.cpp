// RemoteNameTests.cpp — the gate between a server's directory listing and the
// local filesystem. Every case here is a name a hostile server can send.
#include <catch2/catch_test_macros.hpp>

#include "../src/ssh/RemoteName.h"

using amber::CheckRemoteName;
using amber::NameCheck;
using amber::PathWithin;

TEST_CASE("ordinary names are accepted", "[remotename]")
{
    REQUIRE(CheckRemoteName("readme.txt") == NameCheck::Ok);
    REQUIRE(CheckRemoteName("a") == NameCheck::Ok);
    REQUIRE(CheckRemoteName(".bashrc") == NameCheck::Ok);
    REQUIRE(CheckRemoteName("two words.tar.gz") == NameCheck::Ok);
    REQUIRE(CheckRemoteName("-rf") == NameCheck::Ok);          // a shell problem, not a path one
    REQUIRE(CheckRemoteName("\xC3\xA9t\xC3\xA9.txt") == NameCheck::Ok);   // UTF-8 stays legal
    REQUIRE(CheckRemoteName(std::string(255, 'a')) == NameCheck::Ok);
}

TEST_CASE("traversal is refused", "[remotename]")
{
    REQUIRE(CheckRemoteName("..") == NameCheck::Dot);
    REQUIRE(CheckRemoteName(".") == NameCheck::Dot);
    REQUIRE(CheckRemoteName("../evil") == NameCheck::Separator);
    REQUIRE(CheckRemoteName("..\\evil") == NameCheck::Separator);
    REQUIRE(CheckRemoteName("..\\..\\..\\Startup\\evil.exe") == NameCheck::Separator);
    REQUIRE(CheckRemoteName("sub/file") == NameCheck::Separator);
    REQUIRE(CheckRemoteName("/etc/passwd") == NameCheck::Separator);
}

TEST_CASE("Windows-specific names are refused", "[remotename]")
{
    // A drive letter, and an NTFS alternate data stream.
    REQUIRE(CheckRemoteName("C:evil") == NameCheck::DriveOrStream);
    REQUIRE(CheckRemoteName("notes.txt:hidden") == NameCheck::DriveOrStream);
    // Device names resolve in every directory, with any extension.
    REQUIRE(CheckRemoteName("CON") == NameCheck::Reserved);
    REQUIRE(CheckRemoteName("con") == NameCheck::Reserved);
    REQUIRE(CheckRemoteName("COM1.txt") == NameCheck::Reserved);
    REQUIRE(CheckRemoteName("LPT9") == NameCheck::Reserved);
    REQUIRE(CheckRemoteName("NUL") == NameCheck::Reserved);
    // COM0 is not a device.
    REQUIRE(CheckRemoteName("COM0") == NameCheck::Ok);
    REQUIRE(CheckRemoteName("CONSOLE") == NameCheck::Ok);
    // Windows strips these, so "a.txt " and "a.txt" become the same file.
    REQUIRE(CheckRemoteName("a.txt ") == NameCheck::TrailingDotSpace);
    REQUIRE(CheckRemoteName("a.txt.") == NameCheck::TrailingDotSpace);
    REQUIRE(CheckRemoteName("*.txt") == NameCheck::Wildcard);
    REQUIRE(CheckRemoteName("a?b") == NameCheck::Wildcard);
}

TEST_CASE("control characters and lengths are refused", "[remotename]")
{
    REQUIRE(CheckRemoteName("") == NameCheck::Empty);
    REQUIRE(CheckRemoteName(std::string("a\0b", 3)) == NameCheck::Control);
    REQUIRE(CheckRemoteName("a\nb") == NameCheck::Control);
    REQUIRE(CheckRemoteName("a\x1b[31mb") == NameCheck::Control);   // no escapes into the UI
    REQUIRE(CheckRemoteName("a\x7f") == NameCheck::Control);
    REQUIRE(CheckRemoteName(std::string(256, 'a')) == NameCheck::TooLong);
}

TEST_CASE("relative paths are checked component by component", "[remotename]")
{
    using amber::CheckRemotePath;
    std::string bad;
    REQUIRE(CheckRemotePath("a/b/c.txt") == NameCheck::Ok);
    REQUIRE(CheckRemotePath("a.txt") == NameCheck::Ok);

    REQUIRE(CheckRemotePath("") == NameCheck::Empty);
    REQUIRE(CheckRemotePath("/etc/passwd") == NameCheck::Separator);   // absolute
    REQUIRE(CheckRemotePath("\\evil") == NameCheck::Separator);

    // One bad component anywhere in the path condemns the whole path, and the
    // caller is told which one.
    REQUIRE(CheckRemotePath("a/../../evil", &bad) == NameCheck::Dot);
    REQUIRE(bad == "..");
    REQUIRE(CheckRemotePath("a/CON/b", &bad) == NameCheck::Reserved);
    REQUIRE(bad == "CON");
    REQUIRE(CheckRemotePath("a//b", &bad) == NameCheck::Empty);        // empty component
    REQUIRE(CheckRemotePath("a/b:c", &bad) == NameCheck::DriveOrStream);
    REQUIRE(bad == "b:c");
    REQUIRE(CheckRemotePath("ok/then\x1b[31m", &bad) == NameCheck::Control);
}

TEST_CASE("every refusal has a reason string", "[remotename]")
{
    const NameCheck all[] = {
        NameCheck::Ok, NameCheck::Empty, NameCheck::Dot, NameCheck::Separator,
        NameCheck::DriveOrStream, NameCheck::Wildcard, NameCheck::Control,
        NameCheck::Reserved, NameCheck::TrailingDotSpace, NameCheck::TooLong,
        NameCheck::BidiOverride,
    };
    for (NameCheck c : all)
    {
        const char* s = amber::NameCheckReason(c);
        REQUIRE(s != nullptr);
        REQUIRE(std::string(s).size() > 4);
    }
}

TEST_CASE("PathWithin accepts what is inside", "[remotename]")
{
    REQUIRE(PathWithin(L"C:\\dl", L"C:\\dl\\a.txt"));
    REQUIRE(PathWithin(L"C:\\dl", L"C:\\dl\\sub\\a.txt"));
    REQUIRE(PathWithin(L"C:\\dl", L"C:\\dl"));                 // the root itself
    REQUIRE(PathWithin(L"C:\\dl\\", L"C:\\dl\\a.txt"));        // trailing separator
    REQUIRE(PathWithin(L"C:\\dl", L"C:/dl/a.txt"));            // forward slashes
    REQUIRE(PathWithin(L"C:\\DL", L"C:\\dl\\a.txt"));          // case-insensitive
    REQUIRE(PathWithin(L"C:\\dl", L"C:\\dl\\sub\\..\\a.txt")); // stays inside after popping
}

TEST_CASE("PathWithin rejects what is outside", "[remotename]")
{
    REQUIRE_FALSE(PathWithin(L"C:\\dl", L"C:\\dl\\..\\evil.exe"));
    REQUIRE_FALSE(PathWithin(L"C:\\dl", L"C:\\dl\\a\\..\\..\\evil.exe"));
    REQUIRE_FALSE(PathWithin(L"C:\\dl", L"C:\\other\\a.txt"));
    REQUIRE_FALSE(PathWithin(L"C:\\dl", L"D:\\dl\\a.txt"));
    // A sibling whose name merely starts with the root's name.
    REQUIRE_FALSE(PathWithin(L"C:\\dl", L"C:\\dlx\\a.txt"));
    REQUIRE_FALSE(PathWithin(L"", L"C:\\dl\\a.txt"));
    // ".." cannot climb out through the root.
    REQUIRE_FALSE(PathWithin(L"C:\\dl", L"C:\\..\\..\\evil"));
}

TEST_CASE("PathWithin handles UNC roots", "[remotename]")
{
    REQUIRE(PathWithin(L"\\\\srv\\share\\dl", L"\\\\srv\\share\\dl\\a.txt"));
    REQUIRE_FALSE(PathWithin(L"\\\\srv\\share\\dl", L"\\\\srv\\share\\other\\a.txt"));
    REQUIRE_FALSE(PathWithin(L"\\\\srv\\share\\dl", L"\\\\srv\\share\\dl\\..\\..\\evil"));
}

TEST_CASE("text-direction overrides are refused", "[remotename]")
{
    // "evil" + U+202E + "gnp.exe" displays as "evilexe.png": the extension the
    // reader sees is not the extension Windows acts on.
    REQUIRE(CheckRemoteName("evil\xE2\x80\xAE" "gnp.exe") == NameCheck::BidiOverride);
    REQUIRE(CheckRemoteName("\xE2\x80\xAA" "a") == NameCheck::BidiOverride);   // U+202A LRE
    REQUIRE(CheckRemoteName("a\xE2\x80\xAD" "b") == NameCheck::BidiOverride);  // U+202D LRO
    REQUIRE(CheckRemoteName("a\xE2\x81\xA6" "b") == NameCheck::BidiOverride);  // U+2066 LRI
    REQUIRE(CheckRemoteName("a\xE2\x81\xA9" "b") == NameCheck::BidiOverride);  // U+2069 PDI
    REQUIRE(amber::CheckRemotePath("ok/evil\xE2\x80\xAE" "gnp.exe") == NameCheck::BidiOverride);

    // Neighbouring code points are ordinary characters and stay legal, as do
    // Arabic and Hebrew names, which carry their own direction and need no
    // override to render correctly.
    REQUIRE(CheckRemoteName("a\xE2\x80\xA9" "b") == NameCheck::Ok);   // U+2029
    REQUIRE(CheckRemoteName("a\xE2\x81\xAA" "b") == NameCheck::Ok);   // U+206A
    REQUIRE(CheckRemoteName("\xD9\x85\xD9\x84\xD9\x81.txt") == NameCheck::Ok);
    REQUIRE(CheckRemoteName("\xD7\xA7\xD7\x95\xD7\xA8\xD7\x90.txt") == NameCheck::Ok);
}
