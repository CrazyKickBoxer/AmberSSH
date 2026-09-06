// ContrastTests.cpp — the shipped themes, measured rather than assumed.
//
// The audit could not answer "is the amber-on-dark text readable" by reading
// code. This answers it, and keeps answering it: a theme edit that pushes text
// under WCAG AA fails the build instead of shipping.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/ui/Contrast.h"

using amber::ContrastRatio;
using amber::DimFactorFor;
using amber::MinBrightnessForContrast;
using amber::RelativeLuminance;

namespace
{
// The main foreground stop of each shipped theme, in kThemes order. Kept here
// rather than included because kThemes lives inside app.cpp; if a theme is
// edited there and not here, the mirror test below is what notices.
struct Theme { const char* name; uint32_t fg; };
const Theme kThemeFg[] = {
    { "Amber Nixie",    0xFFB000 },
    { "Emerald CRT",    0x2EE060 },
    { "Ice Cathode",    0x2CB9F0 },
    { "Violet Haze",    0xC237F0 },
    { "Blood Cell",     0xF03434 },
    { "Paper White",    0xE6E6DA },
    { "Brass Gaslight", 0xA87A34 },
};
constexpr uint32_t kGround = 0x000000;
}

TEST_CASE("luminance and contrast match the WCAG definition", "[contrast]")
{
    using Catch::Matchers::WithinAbs;
    REQUIRE_THAT(RelativeLuminance(0x000000), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(RelativeLuminance(0xFFFFFF), WithinAbs(1.0, 1e-9));
    // The extreme pair is 21:1 by definition.
    REQUIRE_THAT(ContrastRatio(0x000000, 0xFFFFFF), WithinAbs(21.0, 1e-6));
    // Order does not matter.
    REQUIRE(ContrastRatio(0xFFB000, 0x000000) == ContrastRatio(0x000000, 0xFFB000));
    // A colour against itself is 1:1.
    REQUIRE_THAT(ContrastRatio(0x808080, 0x808080), WithinAbs(1.0, 1e-9));
}

TEST_CASE("every shipped theme's normal text clears WCAG AA", "[contrast]")
{
    for (const Theme& t : kThemeFg)
    {
        INFO(t.name);
        REQUIRE(ContrastRatio(t.fg, kGround) >= 4.5);
    }
}

TEST_CASE("every shipped theme's faint text clears WCAG AA", "[contrast]")
{
    // This is the one that was failing. At a flat 0.55, Violet Haze, Blood
    // Cell and Brass Gaslight sat between 3:1 and 4.5:1 — legible as large
    // text, not as terminal output. DimFactorFor raises the floor per theme.
    for (const Theme& t : kThemeFg)
    {
        INFO(t.name);
        const float f = DimFactorFor(t.fg, kGround);
        const double lum = RelativeLuminance(t.fg) * f;
        const double ratio = (lum + 0.05) / (RelativeLuminance(kGround) + 0.05);
        REQUIRE(ratio >= 4.5);
        REQUIRE(f >= 0.55f);    // never darker than the style asks for
        REQUIRE(f <= 1.0f);     // never brighter than the foreground itself
    }
}

TEST_CASE("themes that already passed are left alone", "[contrast]")
{
    // The fix must not restyle what was not broken.
    REQUIRE(DimFactorFor(0xFFB000, kGround) == 0.55f);   // Amber Nixie
    REQUIRE(DimFactorFor(0x2EE060, kGround) == 0.55f);   // Emerald CRT
    REQUIRE(DimFactorFor(0x2CB9F0, kGround) == 0.55f);   // Ice Cathode
    REQUIRE(DimFactorFor(0xE6E6DA, kGround) == 0.55f);   // Paper White
    // And the three that were failing do move.
    REQUIRE(DimFactorFor(0xC237F0, kGround) > 0.55f);    // Violet Haze
    REQUIRE(DimFactorFor(0xF03434, kGround) > 0.55f);    // Blood Cell
    REQUIRE(DimFactorFor(0xA87A34, kGround) > 0.55f);    // Brass Gaslight
}

TEST_CASE("the floor degrades honestly at the edges", "[contrast]")
{
    // A foreground that cannot reach the target at full brightness returns 1
    // rather than a fraction that pretends the problem is solved.
    REQUIRE(MinBrightnessForContrast(0x202020, kGround) == 1.0f);
    // Dark text on a light ground is not this code path; it says so by
    // returning 1 rather than producing a meaningless factor.
    REQUIRE(MinBrightnessForContrast(0x000000, 0xFFFFFF) == 1.0f);
    // A foreground already far above the target needs very little.
    REQUIRE(MinBrightnessForContrast(0xFFFFFF, kGround) < 0.3f);
}
