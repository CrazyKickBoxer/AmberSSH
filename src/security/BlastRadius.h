// BlastRadius.h — deterministic risk analysis of a command before it runs.
//
// The roadmap is explicit and this file obeys it: an LLM is never the
// authority on whether a destructive command is safe. Everything here is a
// hand-written parser over the exact command text, and every answer it cannot
// establish comes back as "unable to determine" rather than a guess.
//
// This module PARSES. It never executes anything, never rewrites the user's
// command, and never decides on its own to block one — it returns a risk
// level and a list of findings, and the caller decides what confirmation that
// deserves. Keeping the judgement here and the consequences there is what
// makes the judgement testable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

enum class RiskLevel
{
    None = 0,     // nothing recognised as destructive
    Low,          // reversible, or scoped to something small
    Medium,       // loses work, but locally and recoverably
    High,         // deletes or overwrites data, or restarts a service
    Critical,     // irreversible and wide: root paths, force-push, volumes
};

const char* RiskLevelName(RiskLevel r);

// What the user has asked to be warned about. Mirrors the roadmap's menu.
enum class RiskPolicy
{
    Off = 0,
    CriticalOnly,
    HighAndCritical,
    Standard,       // Medium and above
    Everything,     // includes Low — for a production profile
};

const char* RiskPolicyName(RiskPolicy p);

// A single reason the command was flagged. Kept separate from the level so a
// dialog can list what it found rather than asserting a verdict.
struct RiskFinding
{
    std::string what;        // "deletes recursively", "affects a root path"
    std::string detail;      // the argument or path it is about
    RiskLevel level = RiskLevel::Low;
    // True when the finding rests on something the parser could not fully
    // determine — a variable, a glob, a subshell. The UI must say so rather
    // than presenting it as fact.
    bool uncertain = false;
};

struct RiskReport
{
    RiskLevel level = RiskLevel::None;
    std::string command;              // the EXACT original, never rewritten
    std::string program;              // the first word, resolved through sudo
    bool sudo = false;
    std::vector<RiskFinding> findings;
    // True when the command contains something the parser deliberately does
    // not try to understand — a pipe into a shell, command substitution, a
    // variable in a path. The report is then explicitly incomplete.
    bool incomplete = false;
    std::string incompleteWhy;

    bool Flagged(RiskPolicy p) const;
    // The single line a confirmation dialog leads with.
    std::string Headline() const;
};

// The heart of it. Splits the command respecting quotes, walks the argument
// list for the families the roadmap lists, and reports.
//
// `command` is analysed exactly as given. It is never modified, and the
// caller must show the original text alongside any warning.
RiskReport AnalyseCommand(const std::string& command);

// How a confirmation should be demanded. Deliberate friction is reserved for
// Critical: making every warning require typing is how people learn to type
// without reading.
enum class ConfirmStyle
{
    None,          // below the policy threshold: do not interrupt
    Notice,        // a line in the status bar, no modal
    YesNo,         // an ordinary confirmation
    TypeHostname,  // type the host name to continue
};

ConfirmStyle ConfirmFor(RiskLevel level, RiskPolicy policy);

// ---------------------------------------------------------------- helpers
// Splits a command line the way a POSIX shell would for the purposes of
// reading it: honours single and double quotes and backslash escapes, and
// reports whether anything was left unterminated.
std::vector<std::string> SplitArgs(const std::string& command, bool& unterminated);

// True for a path that names a filesystem root or a directory whose loss
// would be catastrophic — "/", "/etc", "/usr", "C:\", a bare "~", and the
// forms that reach them by traversal.
bool IsRootLikePath(const std::string& path);

// True when a token contains shell metacharacters the parser does not model:
// substitution, expansion, a pipe into another program.
bool HasUnmodelledSyntax(const std::string& token);

} // namespace amber
