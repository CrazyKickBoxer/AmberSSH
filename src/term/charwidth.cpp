// charwidth.cpp — sorted range tables + binary search. Emoji ranges follow
// Unicode 15.1 emoji-data.txt (Emoji_Presentation=Yes); wide ranges follow
// EastAsianWidth.txt (W + F). Both mirror what modern glibc wcwidth reports,
// which is what the remote shell uses for cursor math.
#include "charwidth.h"

#include <algorithm>
#include <iterator>

namespace
{

struct Range
{
    char32_t lo, hi;
};

bool InRanges(const Range* table, size_t n, char32_t cp)
{
    const Range* end = table + n;
    const Range* it = std::upper_bound(
        table, end, cp, [](char32_t v, const Range& r) { return v < r.lo; });
    return it != table && cp <= (it - 1)->hi;
}

// Emoji_Presentation=Yes — chars that are color emoji by default.
constexpr Range kEmojiPresentation[] = {
    { 0x231A, 0x231B },   // watch, hourglass
    { 0x23E9, 0x23EC },   // fast-forward block
    { 0x23F0, 0x23F0 },   // alarm clock
    { 0x23F3, 0x23F3 },   // hourglass flowing
    { 0x25FD, 0x25FE },   // small squares
    { 0x2614, 0x2615 },   // umbrella rain, hot beverage
    { 0x2648, 0x2653 },   // zodiac
    { 0x267F, 0x267F },   // wheelchair
    { 0x2693, 0x2693 },   // anchor
    { 0x26A1, 0x26A1 },   // high voltage
    { 0x26AA, 0x26AB },   // circles
    { 0x26BD, 0x26BE },   // soccer, baseball
    { 0x26C4, 0x26C5 },   // snowman, sun behind cloud
    { 0x26CE, 0x26CE },   // ophiuchus
    { 0x26D4, 0x26D4 },   // no entry
    { 0x26EA, 0x26EA },   // church
    { 0x26F2, 0x26F3 },   // fountain, golf
    { 0x26F5, 0x26F5 },   // sailboat
    { 0x26FA, 0x26FA },   // tent
    { 0x26FD, 0x26FD },   // fuel pump
    { 0x2705, 0x2705 },   // check mark button
    { 0x270A, 0x270B },   // fists
    { 0x2728, 0x2728 },   // sparkles
    { 0x274C, 0x274C },   // cross mark
    { 0x274E, 0x274E },   // cross mark button
    { 0x2753, 0x2755 },   // question/exclamation ornaments
    { 0x2757, 0x2757 },   // exclamation mark
    { 0x2795, 0x2797 },   // plus/minus/divide
    { 0x27B0, 0x27B0 },   // curly loop
    { 0x27BF, 0x27BF },   // double curly loop
    { 0x2B1B, 0x2B1C },   // large squares
    { 0x2B50, 0x2B50 },   // star
    { 0x2B55, 0x2B55 },   // hollow red circle
    { 0x1F004, 0x1F004 }, // mahjong red dragon
    { 0x1F0CF, 0x1F0CF }, // joker
    { 0x1F18E, 0x1F18E }, // AB button
    { 0x1F191, 0x1F19A }, // squared CL..VS
    { 0x1F1E6, 0x1F1FF }, // regional indicators (flags; singles render narrow)
    { 0x1F201, 0x1F202 },
    { 0x1F21A, 0x1F21A },
    { 0x1F22F, 0x1F22F },
    { 0x1F232, 0x1F236 },
    { 0x1F238, 0x1F23A },
    { 0x1F250, 0x1F251 },
    { 0x1F300, 0x1F320 },
    { 0x1F32D, 0x1F335 },
    { 0x1F337, 0x1F37C },
    { 0x1F37E, 0x1F393 },
    { 0x1F3A0, 0x1F3CA },
    { 0x1F3CF, 0x1F3D3 },
    { 0x1F3E0, 0x1F3F0 },
    { 0x1F3F4, 0x1F3F4 },
    { 0x1F3F8, 0x1F43E },
    { 0x1F440, 0x1F440 },
    { 0x1F442, 0x1F4FC },
    { 0x1F4FF, 0x1F53D },
    { 0x1F54B, 0x1F54E },
    { 0x1F550, 0x1F567 },
    { 0x1F57A, 0x1F57A },
    { 0x1F595, 0x1F596 },
    { 0x1F5A4, 0x1F5A4 },
    { 0x1F5FB, 0x1F64F },
    { 0x1F680, 0x1F6C5 },
    { 0x1F6CC, 0x1F6CC },
    { 0x1F6D0, 0x1F6D2 },
    { 0x1F6D5, 0x1F6D7 },
    { 0x1F6DC, 0x1F6DF },
    { 0x1F6EB, 0x1F6EC },
    { 0x1F6F4, 0x1F6FC },
    { 0x1F7E0, 0x1F7EB },
    { 0x1F7F0, 0x1F7F0 },
    { 0x1F90C, 0x1F93A },
    { 0x1F93C, 0x1F945 },
    { 0x1F947, 0x1F9FF },
    { 0x1FA70, 0x1FA7C },
    { 0x1FA80, 0x1FA89 },
    { 0x1FA8F, 0x1FAC6 },
    { 0x1FACE, 0x1FADC },
    { 0x1FADF, 0x1FAE9 },
    { 0x1FAF0, 0x1FAF8 },
};

// East Asian Wide + Fullwidth (non-emoji portion): CJK, Hangul, kana, …
constexpr Range kWide[] = {
    { 0x1100, 0x115F },     // Hangul jamo
    { 0x2329, 0x232A },     // angle brackets
    { 0x2E80, 0x303E },     // CJK radicals, kangxi, CJK punctuation
    { 0x3041, 0x33FF },     // kana, CJK compat
    { 0x3400, 0x4DBF },     // CJK ext A
    { 0x4E00, 0x9FFF },     // CJK unified
    { 0xA000, 0xA4CF },     // Yi
    { 0xA960, 0xA97F },     // Hangul jamo ext-A
    { 0xAC00, 0xD7A3 },     // Hangul syllables
    { 0xF900, 0xFAFF },     // CJK compat ideographs
    { 0xFE10, 0xFE19 },     // vertical forms
    { 0xFE30, 0xFE52 },     // CJK compat forms
    { 0xFE54, 0xFE66 },
    { 0xFE68, 0xFE6B },
    { 0xFF00, 0xFF60 },     // fullwidth forms
    { 0xFFE0, 0xFFE6 },
    { 0x16FE0, 0x16FFF },   // Tangut marks
    { 0x17000, 0x187F7 },   // Tangut
    { 0x18800, 0x18AFF },
    { 0x1B000, 0x1B12F },   // kana supplement
    { 0x1B150, 0x1B167 },
    { 0x1F3FB, 0x1F3FF },   // skin-tone modifiers (wide standalone, per glibc)
    { 0x20000, 0x2FFFD },   // CJK ext B+
    { 0x30000, 0x3FFFD },
};

// Zero-width: combining marks, format controls, variation selectors, ZWJ.
constexpr Range kZero[] = {
    { 0x0300, 0x036F },     // combining diacritics
    { 0x0483, 0x0489 },
    { 0x0591, 0x05C7 },     // Hebrew points
    { 0x0610, 0x061A },     // Arabic marks
    { 0x064B, 0x065F },
    { 0x0670, 0x0670 },
    { 0x06D6, 0x06ED },
    { 0x0711, 0x0711 },
    { 0x0730, 0x074A },
    { 0x07A6, 0x07B0 },
    { 0x0E31, 0x0E31 },     // Thai vowels/tones (common combining subset)
    { 0x0E34, 0x0E3A },
    { 0x0E47, 0x0E4E },
    { 0x1AB0, 0x1AFF },     // combining extended
    { 0x1DC0, 0x1DFF },     // combining supplement
    { 0x200B, 0x200F },     // ZWSP..RLM (includes ZWJ U+200D)
    { 0x2028, 0x202E },     // line/para separators, bidi embeds
    { 0x2060, 0x2064 },     // word joiner, invisibles
    { 0x20D0, 0x20FF },     // combining for symbols
    { 0xFE00, 0xFE0F },     // variation selectors (VS15/VS16)
    { 0xFE20, 0xFE2F },     // combining half marks
    { 0xFEFF, 0xFEFF },     // BOM / ZWNBSP
    { 0xE0100, 0xE01EF },   // variation selectors supplement
};

} // namespace

bool IsEmojiPresentation(char32_t cp)
{
    return InRanges(kEmojiPresentation, std::size(kEmojiPresentation), cp);
}

int TermCharWidth(char32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;   // control — never printed as a cell
    if (cp < 0x0300)
        return 1;   // fast path: Latin-1 and below, no tables needed
    if (InRanges(kZero, std::size(kZero), cp))
        return 0;
    if (InRanges(kWide, std::size(kWide), cp))
        return 2;
    // Regional indicators are the exception in the emoji table: glibc
    // wcwidth reports 1, and the server lays them out that way.
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF)
        return 1;
    if (InRanges(kEmojiPresentation, std::size(kEmojiPresentation), cp))
        return 2;
    return 1;
}
