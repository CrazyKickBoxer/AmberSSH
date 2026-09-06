#include "DownloadPlan.h"

#include "RemoteName.h"

namespace amber
{
namespace
{

std::wstring LocalJoin(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty())
        return name;
    if (dir.back() == L'\\' || dir.back() == L'/')
        return dir + name;
    return dir + L"\\" + name;
}

// UTF-8 to UTF-16 without Windows: the planner is pure, and a name that has
// already passed CheckRemoteName holds no separators, no control characters
// and no surrogate tricks, so a straightforward decode is enough. Malformed
// sequences become U+FFFD rather than being dropped, so two different bad
// names cannot collapse into one path.
std::wstring Widen(const std::string& s)
{
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80)               { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 4; }
        else                         { cp = 0xFFFD; n = 1; }
        if (n > 1)
        {
            if (i + n > s.size()) { cp = 0xFFFD; n = 1; }
            else
            {
                for (size_t k = 1; k < n; ++k)
                {
                    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; n = 1; break; }
                    cp = (cp << 6) | (cc & 0x3Fu);
                }
            }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        if (cp < 0x10000)
            out.push_back(static_cast<wchar_t>(cp));
        else
        {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
        i += n;
    }
    return out;
}

struct Walker
{
    const RemoteLister& list;
    const DownloadLimits& limits;
    const std::wstring& root;
    DownloadPlan& plan;

    void Refuse(const std::string& remote, const char* why)
    {
        plan.refused.push_back({ remote, why });
    }

    // Returns false when a limit has been reached and the walk should stop.
    bool Descend(const std::string& rdir, const std::wstring& ldir, int depth)
    {
        if (depth > limits.maxDepth)
        {
            plan.truncated = true;
            Refuse(rdir, "the tree is deeper than this download will follow");
            return false;
        }
        // Belt and braces: the components were each checked on the way in, so
        // this cannot fail. It is cheap, and it is the assertion that holds
        // even if a future edit adds a path that skips the component check.
        if (!PathWithin(root, ldir))
        {
            Refuse(rdir, "it resolves outside the download folder");
            return true;
        }
        plan.dirs.push_back(ldir);

        std::vector<RemoteEntry> kids;
        if (!list(rdir, kids))
            return true;                 // unlistable: nothing under here

        for (const RemoteEntry& k : kids)
        {
            if (plan.files.size() >= limits.maxFiles)
            {
                plan.truncated = true;
                return false;
            }
            const std::string rpath = RemoteJoin(rdir, k.name);
            const NameCheck nc = CheckRemoteName(k.name);
            if (nc != NameCheck::Ok)
            {
                Refuse(rpath, NameCheckReason(nc));
                continue;
            }
            const std::wstring lpath = LocalJoin(ldir, Widen(k.name));
            if (!PathWithin(root, lpath))
            {
                Refuse(rpath, "it resolves outside the download folder");
                continue;
            }
            if (k.dir)
            {
                // A symlinked directory is not followed. The server chooses
                // where it points, so following one is a loop at best and a
                // way out of the tree at worst.
                if (k.link)
                {
                    Refuse(rpath, "it is a symbolic link to a directory");
                    continue;
                }
                if (!Descend(rpath, lpath, depth + 1))
                    return false;
                continue;
            }
            plan.files.push_back({ rpath, lpath, k.size });
        }
        return true;
    }
};

} // namespace

std::string RemoteJoin(const std::string& dir, const std::string& name)
{
    if (dir.empty())
        return name;
    if (dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

DownloadPlan PlanDownload(const std::string& remoteDir,
                          const std::wstring& localRoot,
                          const RemoteEntry& top,
                          const RemoteLister& list,
                          const DownloadLimits& limits)
{
    DownloadPlan plan;
    const std::string rpath = RemoteJoin(remoteDir, top.name);

    const NameCheck nc = CheckRemoteName(top.name);
    if (nc != NameCheck::Ok)
    {
        plan.refused.push_back({ rpath, NameCheckReason(nc) });
        return plan;
    }
    const std::wstring lpath = LocalJoin(localRoot, Widen(top.name));
    if (!PathWithin(localRoot, lpath))
    {
        plan.refused.push_back({ rpath, "it resolves outside the download folder" });
        return plan;
    }

    if (!top.dir)
    {
        plan.files.push_back({ rpath, lpath, top.size });
        return plan;
    }

    // The top entry is descended even when it is a symlink: the user picked
    // it by name, which is a decision about one directory. Its children are
    // not, which is the automatic case.
    Walker w{ list, limits, localRoot, plan };
    w.Descend(rpath, lpath, 1);
    return plan;
}

} // namespace amber
