// DownloadPlan.h — deciding every local path a download will write, before
// any of them is opened.
//
// The traversal fix put a gate in front of each path as it was built, inside
// a recursive lambda tangled with a worker thread and a transfer queue. That
// made the gate correct and left it unreachable: nothing could ask "given
// this listing, where would you write?" without a server on the other end.
//
// This is that question as a function. It walks a remote tree through a
// caller-supplied lister, applies the same gate, and returns the paths it
// would write plus the ones it refused and why. The browser plans first and
// transfers second, so the logic that ships is the logic the tests drive.
//
// Pure: no libssh2, no sockets, no Windows file handles. The lister is a
// callback, so a test supplies a hostile tree directly.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace amber
{

// One entry as a server reported it. `name` is a single path component and is
// not trusted: it is what CheckRemoteName is for.
struct RemoteEntry
{
    std::string name;
    bool dir = false;
    bool link = false;
    uint64_t size = 0;
};

struct PlannedFile
{
    std::string  remote;   // '/'-separated absolute remote path
    std::wstring local;    // absolute local path, guaranteed under the root
    uint64_t     size = 0;
};

struct RefusedEntry
{
    std::string remote;    // the remote path as far as it was resolved
    std::string reason;    // why, in a sentence that can go on a status line
};

struct DownloadPlan
{
    std::vector<std::wstring>  dirs;     // to create, parents before children
    std::vector<PlannedFile>   files;
    std::vector<RefusedEntry>  refused;
    bool truncated = false;              // a limit stopped the walk early
};

// Limits, because a server chooses the shape of the tree.
//
// Without them a hostile or merely broken server can serve a tree that is
// deep enough to exhaust the stack or wide enough to queue for ever, and the
// previous walk had neither bound. Reaching one is not an error; it stops the
// walk and sets `truncated` so the caller can say so.
struct DownloadLimits
{
    int    maxDepth = 32;
    size_t maxFiles = 50000;
};

// Lists one remote directory. Returns false when it cannot be listed, which
// is treated as "nothing under here" rather than as a reason to abandon the
// whole plan.
using RemoteLister = std::function<bool(const std::string& dir, std::vector<RemoteEntry>& out)>;

// Plans the download of `top`, found in `remoteDir`, into `localRoot`.
//
// Guarantees, and they are the point of the whole file:
//   * every path in `files` and `dirs` is inside `localRoot`
//   * no symlinked directory is descended
//   * no component that CheckRemoteName refuses ever reaches a path
//   * the walk terminates
DownloadPlan PlanDownload(const std::string& remoteDir,
                          const std::wstring& localRoot,
                          const RemoteEntry& top,
                          const RemoteLister& list,
                          const DownloadLimits& limits = {});

// Joins a remote directory and a component with '/', the way the SFTP side
// spells paths. Exposed because the planner and the browser must agree.
std::string RemoteJoin(const std::string& dir, const std::string& name);

} // namespace amber
