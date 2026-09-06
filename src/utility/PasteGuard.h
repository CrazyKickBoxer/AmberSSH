// PasteGuard.h — the rule for when pasted text has to be confirmed.
//
// Kept as a pure function so it can be tested directly. Pasting text that
// contains a line break submits every complete line the instant it lands,
// which is the most common way a terminal user destroys something; the guard
// shows what is about to run and waits for an answer.
#pragma once

#include <cstddef>
#include <string>

namespace amber
{

// Anything longer than this is worth a look even without a line break: it is
// almost always an accident (a whole file, a page of logs) rather than intent.
inline constexpr size_t kPasteConfirmChars = 2000;

// `norm` is the text AFTER newline normalisation, so a submitted line ends in
// '\r'. Bracketed paste is deliberately not consulted: plenty of shells and
// full-screen programs never enable it, and the ones that do still run the
// text as soon as Enter is pressed.
inline bool PasteNeedsConfirm(const std::string& norm, bool guardEnabled)
{
    if (!guardEnabled || norm.empty())
        return false;
    // An escape in the payload is worth showing even when nothing else here
    // fires: it means the text was built to do something other than be text.
    return norm.find('\r') != std::string::npos ||
           norm.find('\x1b') != std::string::npos ||
           norm.size() > kPasteConfirmChars;
}

// Pasted text with every escape character removed.
//
// A paste is wrapped in "\e[200~ ... \e[201~" when the shell has bracketed
// paste on. An ESC inside the payload closes that bracket early, and whatever
// follows arrives as ordinary typed input — which is how a copied command
// from a web page runs something the user never saw. The confirm predicate
// above cannot catch it: a payload of "\e[201~curl x|sh" has no line break
// and is nowhere near the length limit.
//
// ESC is dropped rather than escaped. There is no representation of a literal
// ESC that a shell reads as text, so passing one through is never what the
// person pasting wanted; the other C0 controls are left alone because tab and
// the like are ordinary things to paste.
inline std::string PasteWithoutEscapes(const std::string& norm)
{
    std::string out;
    out.reserve(norm.size());
    for (char c : norm)
        if (c != '\x1b')
            out.push_back(c);
    return out;
}

// Whether the above would change anything, so the guard can say so.
inline bool PasteHasEscapes(const std::string& norm)
{
    return norm.find('\x1b') != std::string::npos;
}

// How many lines the paste would submit — what the guard reports.
inline int PasteLineCount(const std::string& norm)
{
    int lines = 1;
    for (char c : norm)
        if (c == '\r')
            ++lines;
    return lines;
}

} // namespace amber
