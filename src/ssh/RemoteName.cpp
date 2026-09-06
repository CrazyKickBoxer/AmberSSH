#include "RemoteName.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace amber
{
namespace
{

// The MS-DOS device names, which Windows still resolves in any directory and
// with any extension: "COM1.txt" is the serial port, not a file. Compared
// against the stem, so the extension does not matter.
bool IsReservedStem(const std::string& stem)
{
    static const char* const kNames[] = { "CON", "PRN", "AUX", "NUL" };
    std::string up;
    up.reserve(stem.size());
    for (char c : stem)
        up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const char* n : kNames)
        if (up == n)
            return true;
    // COM1..COM9 and LPT1..LPT9. COM0 and LPT0 are not devices.
    if (up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) &&
        up[3] >= '1' && up[3] <= '9')
        return true;
    return false;
}

std::wstring ToLowerAscii(std::wstring s)
{
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

// Separators normalised, "." dropped, ".." popped. The leading root — a drive
// ("C:"), a UNC share ("\\\\server\\share") or nothing — is kept aside so a
// ".." can never eat it.
std::wstring Canonical(const std::wstring& in)
{
    std::wstring p = in;
    for (wchar_t& c : p)
        if (c == L'/')
            c = L'\\';

    std::wstring root;
    size_t i = 0;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
    {
        // UNC: keep "\\\\server\\share" whole.
        size_t seg = 2;
        for (int taken = 0; taken < 2 && seg <= p.size(); ++taken)
        {
            size_t next = p.find(L'\\', seg);
            if (next == std::wstring::npos)
            {
                seg = p.size();
                break;
            }
            seg = next + 1;
        }
        root = p.substr(0, seg);
        i = seg;
    }
    else if (p.size() >= 2 && p[1] == L':')
    {
        root = p.substr(0, 2);
        i = 2;
        if (i < p.size() && p[i] == L'\\')
        {
            root += L'\\';
            ++i;
        }
    }
    else if (!p.empty() && p[0] == L'\\')
    {
        root = L"\\";
        i = 1;
    }

    std::vector<std::wstring> parts;
    while (i <= p.size())
    {
        size_t next = p.find(L'\\', i);
        const std::wstring seg =
            p.substr(i, next == std::wstring::npos ? std::wstring::npos : next - i);
        if (!seg.empty() && seg != L".")
        {
            if (seg == L".." && !parts.empty())
                parts.pop_back();
            else if (seg != L"..")
                parts.push_back(seg);
            // A ".." with nothing to pop is dropped: it cannot climb past the
            // root, which is the whole point of keeping the root aside.
        }
        if (next == std::wstring::npos)
            break;
        i = next + 1;
    }

    std::wstring out = root;
    for (size_t k = 0; k < parts.size(); ++k)
    {
        if (!out.empty() && out.back() != L'\\')
            out += L'\\';
        out += parts[k];
    }
    return out;
}

} // namespace

NameCheck CheckRemoteName(const std::string& name)
{
    if (name.empty())
        return NameCheck::Empty;
    if (name == "." || name == "..")
        return NameCheck::Dot;
    // 255 is the per-component limit on every filesystem Windows mounts.
    if (name.size() > 255)
        return NameCheck::TooLong;

    for (char ch : name)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F)
            return NameCheck::Control;
        if (c == '/' || c == '\\')
            return NameCheck::Separator;
        if (c == ':')
            return NameCheck::DriveOrStream;
        if (c == '*' || c == '?')
            return NameCheck::Wildcard;
    }

    if (name.back() == '.' || name.back() == ' ')
        return NameCheck::TrailingDotSpace;

    const size_t dot = name.find('.');
    if (IsReservedStem(dot == std::string::npos ? name : name.substr(0, dot)))
        return NameCheck::Reserved;

    return NameCheck::Ok;
}

NameCheck CheckRemotePath(const std::string& path, std::string* badComponent)
{
    if (badComponent)
        badComponent->clear();
    if (path.empty())
        return NameCheck::Empty;
    // An absolute path is not relative to anything, so it is not a valid
    // answer to "where under the sync root does this go".
    if (path.front() == '/' || path.front() == '\\')
    {
        if (badComponent)
            *badComponent = path;
        return NameCheck::Separator;
    }
    size_t i = 0;
    while (i <= path.size())
    {
        const size_t next = path.find('/', i);
        const std::string seg =
            path.substr(i, next == std::string::npos ? std::string::npos : next - i);
        const NameCheck c = CheckRemoteName(seg);
        if (c != NameCheck::Ok)
        {
            if (badComponent)
                *badComponent = seg;
            return c;
        }
        if (next == std::string::npos)
            break;
        i = next + 1;
    }
    return NameCheck::Ok;
}

const char* NameCheckReason(NameCheck c)
{
    switch (c)
    {
    case NameCheck::Ok:               return "accepted";
    case NameCheck::Empty:            return "the name is empty";
    case NameCheck::Dot:              return "the name is \".\" or \"..\"";
    case NameCheck::Separator:        return "the name contains a path separator";
    case NameCheck::DriveOrStream:    return "the name contains \":\"";
    case NameCheck::Wildcard:         return "the name contains a wildcard";
    case NameCheck::Control:          return "the name contains a control character";
    case NameCheck::Reserved:         return "the name is a reserved device name";
    case NameCheck::TrailingDotSpace: return "the name ends in a dot or a space";
    case NameCheck::TooLong:          return "the name is longer than 255 bytes";
    }
    return "refused";
}

bool PathWithin(const std::wstring& root, const std::wstring& child)
{
    if (root.empty())
        return false;
    std::wstring r = ToLowerAscii(Canonical(root));
    const std::wstring c = ToLowerAscii(Canonical(child));
    while (r.size() > 1 && r.back() == L'\\')
        r.pop_back();
    if (c.size() < r.size())
        return false;
    if (c.compare(0, r.size(), r) != 0)
        return false;
    if (c.size() == r.size())
        return true;                    // the root itself
    // "C:\a" must not match "C:\ab": the next character has to be a separator,
    // unless the root already ended in one (a drive root, "C:\").
    return c[r.size()] == L'\\' || r.back() == L'\\';
}

} // namespace amber
