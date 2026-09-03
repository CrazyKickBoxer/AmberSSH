#include "BlastRadius.h"

#include <algorithm>
#include <cctype>

namespace amber
{

namespace
{

bool Eq(const std::string& a, const char* b)
{
    return a == b;
}

// A flag is present if it appears as its own token, or as a letter inside a
// short-option cluster ("-rf" contains 'r' and 'f'). Long options are matched
// whole, so "--force" is not found inside "--force-with-lease".
bool HasShortFlag(const std::vector<std::string>& args, size_t from, char f)
{
    for (size_t i = from; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        if (a.size() < 2 || a[0] != '-' || a[1] == '-')
            continue;
        if (a.find(f, 1) != std::string::npos)
            return true;
    }
    return false;
}

bool HasLongFlag(const std::vector<std::string>& args, size_t from, const char* f)
{
    for (size_t i = from; i < args.size(); ++i)
        if (args[i] == f)
            return true;
    return false;
}

// The non-flag arguments after `from`.
std::vector<std::string> Operands(const std::vector<std::string>& args, size_t from)
{
    std::vector<std::string> out;
    bool endOfFlags = false;
    for (size_t i = from; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        if (!endOfFlags && a == "--")
        {
            endOfFlags = true;
            continue;
        }
        if (!endOfFlags && !a.empty() && a[0] == '-' && a != "-")
            continue;
        out.push_back(a);
    }
    return out;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void Add(RiskReport& r, const char* what, const std::string& detail,
         RiskLevel level, bool uncertain = false)
{
    RiskFinding f;
    f.what = what;
    f.detail = detail;
    f.level = level;
    f.uncertain = uncertain;
    r.findings.push_back(std::move(f));
    if (static_cast<int>(level) > static_cast<int>(r.level))
        r.level = level;
}

} // namespace

const char* RiskLevelName(RiskLevel r)
{
    switch (r)
    {
    case RiskLevel::None:     return "none";
    case RiskLevel::Low:      return "low";
    case RiskLevel::Medium:   return "medium";
    case RiskLevel::High:     return "high";
    case RiskLevel::Critical: return "critical";
    }
    return "unknown";
}

const char* RiskPolicyName(RiskPolicy p)
{
    switch (p)
    {
    case RiskPolicy::Off:             return "off";
    case RiskPolicy::CriticalOnly:    return "critical only";
    case RiskPolicy::HighAndCritical: return "high and critical";
    case RiskPolicy::Standard:        return "standard";
    case RiskPolicy::Everything:      return "everything";
    }
    return "unknown";
}

// ------------------------------------------------------------------ split
std::vector<std::string> SplitArgs(const std::string& command, bool& unterminated)
{
    unterminated = false;
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    enum { Plain, Single, Double } mode = Plain;
    for (size_t i = 0; i < command.size(); ++i)
    {
        const char c = command[i];
        if (mode == Plain)
        {
            if (c == '\\' && i + 1 < command.size())
            {
                cur.push_back(command[++i]);
                have = true;
                continue;
            }
            if (c == '\'') { mode = Single; have = true; continue; }
            if (c == '"')  { mode = Double; have = true; continue; }
            if (isspace(static_cast<unsigned char>(c)))
            {
                if (have)
                {
                    out.push_back(cur);
                    cur.clear();
                    have = false;
                }
                continue;
            }
            cur.push_back(c);
            have = true;
            continue;
        }
        if (mode == Single)
        {
            // Inside single quotes a backslash is literal, as in a shell.
            if (c == '\'') { mode = Plain; continue; }
            cur.push_back(c);
            continue;
        }
        // Double
        if (c == '\\' && i + 1 < command.size())
        {
            const char n = command[i + 1];
            // Only these are special inside double quotes.
            if (n == '"' || n == '\\' || n == '$' || n == '`')
            {
                cur.push_back(n);
                ++i;
                continue;
            }
            cur.push_back(c);
            continue;
        }
        if (c == '"') { mode = Plain; continue; }
        cur.push_back(c);
    }
    if (have)
        out.push_back(cur);
    unterminated = mode != Plain;
    return out;
}

bool HasUnmodelledSyntax(const std::string& token)
{
    // Command substitution, arithmetic expansion, and variables: the parser
    // cannot know what these expand to, so anything resting on them is
    // reported as uncertain rather than asserted.
    if (token.find("$(") != std::string::npos)
        return true;
    if (token.find("${") != std::string::npos)
        return true;
    if (token.find('`') != std::string::npos)
        return true;
    if (token.find('$') != std::string::npos)
        return true;
    return false;
}

bool IsRootLikePath(const std::string& path)
{
    if (path.empty())
        return false;
    std::string p = path;
    // Strip a trailing slash so "/etc/" and "/etc" are the same answer.
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
        p.pop_back();
    if (p == "/" || p == "~" || p == "/*" || p == "." || p == "..")
        return true;
    // A Windows drive root.
    if (p.size() <= 3 && p.size() >= 2 && isalpha(static_cast<unsigned char>(p[0])) &&
        p[1] == ':')
        return true;
    static const char* kSystem[] = {
        "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib64", "/boot", "/dev",
        "/proc", "/sys", "/var", "/opt", "/home", "/root", "/srv",
    };
    const std::string low = Lower(p);
    for (const char* s : kSystem)
        if (low == s)
            return true;
    // A path that walks out of itself far enough to reach the root. Two
    // levels is normal; four is someone reaching for something.
    int ups = 0;
    for (size_t i = 0; i + 1 < p.size(); ++i)
        if (p[i] == '.' && p[i + 1] == '.')
            ++ups;
    return ups >= 4;
}

// ---------------------------------------------------------------- analyse
namespace
{

void AnalyseRm(RiskReport& r, const std::vector<std::string>& a, size_t from)
{
    const bool recursive = HasShortFlag(a, from, 'r') || HasShortFlag(a, from, 'R') ||
                           HasLongFlag(a, from, "--recursive");
    const bool force = HasShortFlag(a, from, 'f') || HasLongFlag(a, from, "--force");
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.empty())
    {
        Add(r, "deletes, but no target was given", "", RiskLevel::Low);
        return;
    }
    for (const std::string& p : ops)
    {
        const bool unsure = HasUnmodelledSyntax(p);
        if (IsRootLikePath(p))
            Add(r, recursive ? "recursively deletes a system or root path"
                             : "deletes a system or root path",
                p, RiskLevel::Critical, unsure);
        else if (recursive)
            Add(r, "deletes a directory and everything in it", p,
                force ? RiskLevel::High : RiskLevel::High, unsure);
        else
            Add(r, "deletes", p, RiskLevel::Medium, unsure);
        if (p.find('*') != std::string::npos || p.find('?') != std::string::npos)
            Add(r, "the target is a wildcard — what it matches is decided by "
                   "the remote shell, not visible here",
                p, RiskLevel::High, true);
    }
    if (force && recursive)
        Add(r, "force and recursive together: no prompts, no stopping on error",
            "-rf", RiskLevel::High);
}

void AnalyseMvCp(RiskReport& r, const std::vector<std::string>& a, size_t from,
                 bool move)
{
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.size() < 2)
        return;
    const std::string& dest = ops.back();
    const bool noClobber = HasShortFlag(a, from, 'n') ||
                           HasLongFlag(a, from, "--no-clobber");
    if (IsRootLikePath(dest))
        Add(r, move ? "moves onto a system or root path"
                    : "copies onto a system or root path",
            dest, RiskLevel::Critical, HasUnmodelledSyntax(dest));
    else if (!noClobber)
        Add(r, move ? "moves, overwriting anything already at the destination"
                    : "copies, overwriting anything already at the destination",
            dest, RiskLevel::Medium, HasUnmodelledSyntax(dest));
    if (move)
        for (size_t i = 0; i + 1 < ops.size(); ++i)
            if (IsRootLikePath(ops[i]))
                Add(r, "moves a system or root path away", ops[i],
                    RiskLevel::Critical);
}

void AnalyseChmodChown(RiskReport& r, const std::vector<std::string>& a,
                       size_t from, bool own)
{
    const bool recursive = HasShortFlag(a, from, 'R') ||
                           HasLongFlag(a, from, "--recursive");
    const std::vector<std::string> ops = Operands(a, from);
    for (size_t i = (ops.empty() ? 0 : 1); i < ops.size(); ++i)
    {
        if (IsRootLikePath(ops[i]))
            Add(r, own ? "changes ownership of a system or root path"
                       : "changes permissions on a system or root path",
                ops[i], RiskLevel::Critical, HasUnmodelledSyntax(ops[i]));
        else if (recursive)
            Add(r, own ? "changes ownership of a whole tree"
                       : "changes permissions on a whole tree",
                ops[i], RiskLevel::High, HasUnmodelledSyntax(ops[i]));
    }
    if (!own && !ops.empty() && (ops[0] == "777" || ops[0] == "0777"))
        Add(r, "makes the target world-writable", ops[0], RiskLevel::High);
}

void AnalyseSystemctl(RiskReport& r, const std::vector<std::string>& a, size_t from)
{
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.empty())
        return;
    const std::string verb = Lower(ops[0]);
    const std::string unit = ops.size() > 1 ? ops[1] : std::string();
    if (verb == "stop" || verb == "restart" || verb == "kill")
        Add(r, ("interrupts a running service (" + verb + ")").c_str(), unit,
            RiskLevel::High);
    else if (verb == "disable" || verb == "mask")
        Add(r, "stops a service starting again after a reboot", unit,
            RiskLevel::High);
    else if (verb == "isolate" || verb == "poweroff" || verb == "reboot" ||
             verb == "halt")
        Add(r, "affects the whole machine", verb, RiskLevel::Critical);
    else if (verb == "daemon-reload" || verb == "start" || verb == "enable")
        Add(r, ("changes service state (" + verb + ")").c_str(), unit,
            RiskLevel::Low);
}

void AnalyseDocker(RiskReport& r, const std::vector<std::string>& a, size_t from)
{
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.empty())
        return;
    const std::string sub = Lower(ops[0]);
    const bool volumes = HasShortFlag(a, from, 'v') ||
                         HasLongFlag(a, from, "--volumes");
    if (sub == "compose" && ops.size() > 1)
    {
        const std::string sub2 = Lower(ops[1]);
        if (sub2 == "down")
        {
            if (volumes)
                Add(r, "removes named volumes — persistent container data is "
                       "destroyed",
                    "docker compose down -v", RiskLevel::Critical);
            else
                Add(r, "stops and removes the compose containers and networks",
                    "docker compose down", RiskLevel::High);
        }
        return;
    }
    if (sub == "system" && ops.size() > 1 && Lower(ops[1]) == "prune")
        Add(r, volumes ? "prunes containers, images, networks AND volumes"
                       : "prunes unused containers, images and networks",
            "docker system prune", volumes ? RiskLevel::Critical : RiskLevel::High);
    else if (sub == "rm" || sub == "rmi")
        Add(r, "removes containers or images", sub, RiskLevel::High);
    else if (sub == "volume" && ops.size() > 1 &&
             (Lower(ops[1]) == "rm" || Lower(ops[1]) == "prune"))
        Add(r, "removes volumes — persistent container data is destroyed",
            "docker volume", RiskLevel::Critical);
    else if (sub == "stop" || sub == "kill" || sub == "restart")
        Add(r, ("interrupts running containers (" + sub + ")").c_str(), "",
            RiskLevel::High);
}

void AnalyseGit(RiskReport& r, const std::vector<std::string>& a, size_t from)
{
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.empty())
        return;
    const std::string sub = Lower(ops[0]);
    if (sub == "reset" && HasLongFlag(a, from, "--hard"))
        Add(r, "discards every uncommitted change in the working tree",
            "git reset --hard", RiskLevel::High);
    else if (sub == "clean")
    {
        const bool force = HasShortFlag(a, from, 'f');
        const bool dirs = HasShortFlag(a, from, 'd');
        const bool ignored = HasShortFlag(a, from, 'x');
        if (force)
            Add(r, ignored ? "deletes untracked AND ignored files, which "
                             "includes build output and local configuration"
                           : (dirs ? "deletes untracked files and directories"
                                   : "deletes untracked files"),
                "git clean", ignored ? RiskLevel::Critical : RiskLevel::High);
    }
    else if (sub == "push")
    {
        const bool force = HasShortFlag(a, from, 'f') ||
                           HasLongFlag(a, from, "--force");
        const bool lease = HasLongFlag(a, from, "--force-with-lease");
        if (force && !lease)
            Add(r, "force-pushes: rewrites history that other people may "
                   "already have",
                "git push --force", RiskLevel::Critical);
        else if (lease)
            Add(r, "force-pushes with a lease — safer, but still rewrites "
                   "remote history",
                "git push --force-with-lease", RiskLevel::High);
        if (HasLongFlag(a, from, "--delete"))
            Add(r, "deletes a remote branch", "git push --delete",
                RiskLevel::High);
    }
    else if (sub == "branch" &&
             (HasShortFlag(a, from, 'D') || HasLongFlag(a, from, "--delete")))
        Add(r, "deletes a branch", "git branch -D", RiskLevel::Medium);
    else if (sub == "checkout" || sub == "restore" || sub == "switch")
    {
        if (HasLongFlag(a, from, "--force") || HasShortFlag(a, from, 'f'))
            Add(r, "discards local changes to the affected files",
                ("git " + sub).c_str(), RiskLevel::Medium);
    }
}

void AnalyseKubectl(RiskReport& r, const std::vector<std::string>& a, size_t from)
{
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.empty())
        return;
    const std::string sub = Lower(ops[0]);
    const bool allNs = HasLongFlag(a, from, "--all-namespaces") ||
                       HasShortFlag(a, from, 'A');
    const bool all = HasLongFlag(a, from, "--all");
    if (sub == "delete")
        Add(r, (allNs || all) ? "deletes resources across everything it matches"
                              : "deletes cluster resources",
            "kubectl delete", (allNs || all) ? RiskLevel::Critical : RiskLevel::High);
    else if (sub == "drain" || sub == "cordon")
        Add(r, "takes a node out of service", sub, RiskLevel::High);
    else if (sub == "scale")
        Add(r, "changes the replica count", "kubectl scale", RiskLevel::Medium);
    else if (sub == "rollout" && ops.size() > 1 && Lower(ops[1]) == "undo")
        Add(r, "rolls a deployment back to a previous revision", "kubectl rollout undo",
            RiskLevel::High);
}

void AnalysePackages(RiskReport& r, const std::string& prog,
                     const std::vector<std::string>& a, size_t from)
{
    const std::vector<std::string> ops = Operands(a, from);
    if (ops.empty())
        return;
    const std::string sub = Lower(ops[0]);
    const bool removes = sub == "remove" || sub == "purge" || sub == "erase" ||
                         sub == "autoremove" || sub == "uninstall";
    if (!removes)
        return;
    Add(r, sub == "purge" ? "removes packages AND their configuration"
                          : "removes installed packages",
        prog + " " + sub, sub == "purge" ? RiskLevel::High : RiskLevel::Medium);
}

void AnalyseDatabase(RiskReport& r, const std::string& command)
{
    // Only matched when the statement is unmistakable: the roadmap says to
    // act on a database context only when it is explicitly known, and a bare
    // word like "drop" in prose must never trigger this.
    const std::string low = Lower(command);
    struct Pat { const char* text; const char* what; RiskLevel level; };
    static const Pat kPats[] = {
        { "drop database",  "drops an entire database",             RiskLevel::Critical },
        { "drop schema",    "drops a schema",                       RiskLevel::Critical },
        { "drop table",     "drops a table and its data",           RiskLevel::High },
        { "truncate table", "empties a table",                      RiskLevel::High },
        { "delete from",    "deletes rows",                         RiskLevel::Medium },
        { "update ",        "updates rows",                         RiskLevel::Low },
    };
    for (const Pat& p : kPats)
    {
        if (low.find(p.text) == std::string::npos)
            continue;
        // A DELETE or UPDATE with no WHERE hits every row.
        const bool noWhere = low.find(" where ") == std::string::npos;
        RiskLevel lvl = p.level;
        std::string what = p.what;
        if (noWhere && (std::string(p.text) == "delete from" ||
                        std::string(p.text) == "update "))
        {
            lvl = RiskLevel::Critical;
            what += " — with no WHERE clause, so EVERY row";
        }
        Add(r, what.c_str(), p.text, lvl);
        break;
    }
}

} // namespace

RiskReport AnalyseCommand(const std::string& command)
{
    RiskReport r;
    r.command = command;

    bool unterminated = false;
    std::vector<std::string> args = SplitArgs(command, unterminated);
    if (unterminated)
    {
        r.incomplete = true;
        r.incompleteWhy = "the command has an unclosed quote, so it was read as "
                          "far as possible only";
    }
    if (args.empty())
        return r;

    // A pipeline, a redirect into a shell, or a chained command: the parser
    // reads the FIRST command and says the rest was not examined. Pretending
    // to understand a whole pipeline is how a preview becomes misleading.
    for (size_t k = 0; k < args.size(); ++k)
    {
        const std::string& a = args[k];
        if (a == "|" || a == "||" || a == "&&" || a == ";" || a == "|&" ||
            a == "&")
        {
            r.incomplete = true;
            r.incompleteWhy = "only the first command in the chain was examined";
            // Actually stop there. Leaving the tail in would let "rm x | tee y"
            // report "deletes: tee", which is worse than saying nothing: a
            // warning that names the wrong file teaches people to ignore it.
            args.resize(k);
            break;
        }
    }
    if (args.empty())
        return r;
    if (command.find("$(") != std::string::npos ||
        command.find('`') != std::string::npos)
    {
        r.incomplete = true;
        r.incompleteWhy = "the command contains substitution, so what it "
                          "actually runs is not known here";
    }

    size_t i = 0;
    // Walk past env assignments and sudo/doas so "sudo -u x rm -rf /" is read
    // as an rm.
    for (;;)
    {
        if (i >= args.size())
            return r;
        const std::string& a = args[i];
        if (a == "sudo" || a == "doas")
        {
            r.sudo = true;
            ++i;
            // Skip sudo's own options and their values.
            while (i < args.size() && !args[i].empty() && args[i][0] == '-')
            {
                const bool takesValue = args[i] == "-u" || args[i] == "-g" ||
                                        args[i] == "--user" || args[i] == "--group";
                ++i;
                if (takesValue && i < args.size())
                    ++i;
            }
            continue;
        }
        if (a == "env" || a == "nohup" || a == "time")
        {
            ++i;
            continue;
        }
        if (a.find('=') != std::string::npos && a.find('/') == std::string::npos &&
            !a.empty() && (isalpha(static_cast<unsigned char>(a[0])) || a[0] == '_'))
        {
            ++i;    // VAR=value prefix
            continue;
        }
        break;
    }
    if (i >= args.size())
        return r;

    // The program name, without its directory.
    std::string prog = args[i];
    const size_t slash = prog.find_last_of("/\\");
    if (slash != std::string::npos)
        prog = prog.substr(slash + 1);
    prog = Lower(prog);
    r.program = prog;
    const size_t from = i + 1;

    if (Eq(prog, "rm") || Eq(prog, "shred") || Eq(prog, "unlink"))
        AnalyseRm(r, args, from);
    else if (Eq(prog, "mv"))
        AnalyseMvCp(r, args, from, true);
    else if (Eq(prog, "cp") || Eq(prog, "rsync"))
        AnalyseMvCp(r, args, from, false);
    else if (Eq(prog, "chmod"))
        AnalyseChmodChown(r, args, from, false);
    else if (Eq(prog, "chown") || Eq(prog, "chgrp"))
        AnalyseChmodChown(r, args, from, true);
    else if (Eq(prog, "systemctl") || Eq(prog, "service"))
        AnalyseSystemctl(r, args, from);
    else if (Eq(prog, "docker") || Eq(prog, "podman"))
        AnalyseDocker(r, args, from);
    else if (Eq(prog, "git"))
        AnalyseGit(r, args, from);
    else if (Eq(prog, "kubectl") || Eq(prog, "k9s") || Eq(prog, "helm"))
        AnalyseKubectl(r, args, from);
    else if (Eq(prog, "apt") || Eq(prog, "apt-get") || Eq(prog, "dnf") ||
             Eq(prog, "yum") || Eq(prog, "pacman") || Eq(prog, "zypper") ||
             Eq(prog, "pip") || Eq(prog, "npm"))
        AnalysePackages(r, prog, args, from);
    else if (Eq(prog, "mkfs") || prog.rfind("mkfs.", 0) == 0)
        Add(r, "formats a filesystem — everything on the target is destroyed",
            prog, RiskLevel::Critical);
    else if (Eq(prog, "dd"))
    {
        for (const std::string& a : args)
            if (a.rfind("of=", 0) == 0)
                Add(r, "writes directly to a device or file, destroying what is "
                       "there",
                    a, RiskLevel::Critical);
    }
    else if (Eq(prog, "shutdown") || Eq(prog, "reboot") || Eq(prog, "halt") ||
             Eq(prog, "poweroff") || Eq(prog, "init"))
        Add(r, "affects the whole machine", prog, RiskLevel::Critical);
    else if (Eq(prog, "psql") || Eq(prog, "mysql") || Eq(prog, "mariadb") ||
             Eq(prog, "sqlite3") || Eq(prog, "mongosh"))
        AnalyseDatabase(r, command);
    else
        AnalyseDatabase(r, command);   // a bare SQL statement typed at a prompt

    // sudo does not create risk on its own, but it removes the safety net of
    // a permission error, so a flagged command run as root is one step worse.
    if (r.sudo && r.level != RiskLevel::None && r.level != RiskLevel::Critical)
    {
        Add(r, "runs as root, so a permission error will not stop it", "sudo",
            r.level);
        r.level = static_cast<RiskLevel>(static_cast<int>(r.level) + 1);
    }
    return r;
}

bool RiskReport::Flagged(RiskPolicy p) const
{
    switch (p)
    {
    case RiskPolicy::Off:             return false;
    case RiskPolicy::CriticalOnly:    return level == RiskLevel::Critical;
    case RiskPolicy::HighAndCritical: return static_cast<int>(level) >=
                                             static_cast<int>(RiskLevel::High);
    case RiskPolicy::Standard:        return static_cast<int>(level) >=
                                             static_cast<int>(RiskLevel::Medium);
    case RiskPolicy::Everything:      return level != RiskLevel::None;
    }
    return false;
}

std::string RiskReport::Headline() const
{
    if (findings.empty())
        return "Nothing recognised as destructive.";
    // The most severe finding leads, because that is what the decision is
    // actually about.
    const RiskFinding* worst = &findings[0];
    for (const RiskFinding& f : findings)
        if (static_cast<int>(f.level) > static_cast<int>(worst->level))
            worst = &f;
    std::string out = "This ";
    out += worst->what;
    if (!worst->detail.empty())
        out += ": " + worst->detail;
    if (worst->uncertain)
        out += "  (unable to determine exactly what this expands to)";
    return out;
}

ConfirmStyle ConfirmFor(RiskLevel level, RiskPolicy policy)
{
    RiskReport probe;
    probe.level = level;
    if (!probe.Flagged(policy))
        return ConfirmStyle::None;
    switch (level)
    {
    case RiskLevel::Critical: return ConfirmStyle::TypeHostname;
    case RiskLevel::High:     return ConfirmStyle::YesNo;
    case RiskLevel::Medium:   return ConfirmStyle::YesNo;
    case RiskLevel::Low:      return ConfirmStyle::Notice;
    case RiskLevel::None:     return ConfirmStyle::None;
    }
    return ConfirmStyle::None;
}

} // namespace amber
