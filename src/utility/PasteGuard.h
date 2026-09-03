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
    return norm.find('\r') != std::string::npos ||
           norm.size() > kPasteConfirmChars;
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
