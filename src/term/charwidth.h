// charwidth.h — terminal display width of a codepoint (wcwidth analogue).
// The width table MUST agree with the server's wcwidth: the remote shell
// positions the cursor assuming these widths, and any disagreement desyncs
// every erase/redraw that follows an emoji or CJK character.
#pragma once

#include <cstdint>

// 0 = zero-width (combining marks, ZWJ, variation selectors — no cell),
// 1 = normal, 2 = wide (East Asian Wide/Fullwidth + emoji-presentation).
int TermCharWidth(char32_t cp);

// True for codepoints whose default presentation is COLOR emoji
// (Unicode Emoji_Presentation=Yes). Text-default symbols (✔ ❤ ➜ …) are
// not included — they render mono unless followed by VS16 (U+FE0F).
bool IsEmojiPresentation(char32_t cp);
