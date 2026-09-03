// Chrome.h — the interface skin ("chrome style"): shape language and colour
// set shared by every UI surface — the main window's title strip and tabs,
// the connection manager and the other native dialogs.
//
// A style is data (ChromeSpec): tab lean, corner chamfer or pill ends, HUD
// brackets, scanlines, elbow frame, dark-on-colour text, and either its own
// palette or accents derived from the terminal theme. Adding a skin means
// adding one ChromeSpec to the table in Chrome.cpp and, only when it needs a
// new silhouette, one shape mode in rects.hlsl / the dialog's SkinShape.
#pragma once

#include <cstdint>

namespace amber
{

enum class ChromeStyle : int
{
    Cyberpunk = 0,   // neon cyan / magenta, chamfers, leaning tabs, HUD ticks
    Classic = 1,     // the Termius-style strip in the terminal theme's colours
    Lcars = 2,       // Star Trek LCARS: pill segments, elbow frame, black text
    Nostromo = 3,    // ship-computer green phosphor, blocky, heavy scanlines
    Blueprint = 4,   // cyanotype drafting sheet: hairline outlines, no fills
    Brass = 5,       // nixie instrument panel: walnut, brass bars, amber digits
    Swiss = 6,       // light paper ground, black type, one red accent
    NeoTokyo = 7,    // red on black, hard slants, hazard striping
    Steampunk = 8,   // coal and walnut, brass trim, gaslight, riveted plates
    SolderMask = 9,  // PCB: mask green, gold pads, silkscreen caps, routed traces
    Horologe = 10,   // watch dial: rhodium indices, lume, a rehaut minute track
    Letterpress = 11,// one-ink press on cotton stock: deboss, foil, printer's rules
    Atelier = 12,    // full-grain leather: saddle stitch, edge paint, a brass rivet
    Reference = 13,  // boutique hi-fi: brushed aluminium, VU meters, engraving
    Tenmoku = 14,    // studio pottery: iron glaze, oil spots, a raw stoneware foot
    Count
};

struct ChromeSpec
{
    const char* name;
    // ---- shape language --------------------------------------------------
    float tabSlant;        // px each tab edge leans (0 = square tabs)
    float chamfer;         // px cut off button / field corners (0 = none)
    bool  pills;           // fully rounded ends on tabs, buttons, segments
    bool  elbow;           // LCARS frame: side bar joined to the top bar
    bool  darkText;        // black text on coloured bars (else light text)
    bool  condensedFont;   // condensed sans for dialog text
    bool  hudBrackets;     // corner ticks on fields and panels
    bool  scanlines;       // faint hairlines across strips and panels
    bool  numberedTabs;    // "01" index tag before each tab caption
    bool  glow;            // additive neon glow on active/hovered pieces
    // Hairline drafting look: shapes are drawn as outlines of this many
    // logical px with no fill (0 = filled, the normal case).
    float outline;
    // The chrome ground is LIGHT. Flips the window caption out of dark mode,
    // picks dark text on accents, and softens the shadows that assume a dark
    // strip. Independent of the terminal's own appearance mode.
    bool  lightGround;
    // Diagonal hazard striping on destructive controls (close, Delete).
    bool  hazard;
    // Brass filigree, gears and rails on dialog grounds and window strips.
    // Never drawn over the terminal grid.
    bool  ornament;
    // Copper routing: dividers and panel edges are traces with 45 degree
    // mitres and vias, and controls carry silkscreen reference designators.
    bool  traces;
    // Haute horlogerie: a rehaut minute track on every long edge, applied
    // indices raised by a highlight and a shade rather than filled, and
    // clous-de-Paris guilloché on the header band and the About panel.
    bool  rehaut;
    // Letterpress: shapes are pressed INTO the stock (a shade top-left, a
    // light bottom-right); inactive controls carry the impression and no ink.
    bool  impression;
    // Atelier: outlines are a saddle stitch inside a painted edge, the active
    // piece is riveted, and fields are lined with a check.
    bool  stitch;
    // Reference: brushed plate, engraved lettering, LEDs, and a VU meter for
    // anything that is a level.
    bool  meter;
    // Tenmoku: fills pool darker at the edge, glazed pieces have a rust rim,
    // every panel stands on a raw stoneware foot, the active piece is chopped.
    bool  glaze;
    // Render chrome labels in capitals (tabs, buttons, tree, menu, fields).
    bool  uppercase;
    // Face for chrome text, or nullptr for the system UI font. Takes
    // precedence over condensedFont, which only picks a condensed default.
    const wchar_t* uiFont;
    // The face the skin WANTS — a boutique family fetched on first use — or
    // nullptr when uiFont already is the face. Resolved through ChromeFace().
    const wchar_t* uiFontWanted;
    // ---- colours, sRGB 0xRRGGBB ------------------------------------------
    // useThemeAccent: derive every colour from the terminal theme instead of
    // the values below (the Classic look).
    bool     useThemeAccent;
    uint32_t bg;           // window / strip ground
    uint32_t panel;        // raised panels, inactive tabs
    uint32_t field;        // edit wells
    uint32_t neonA;        // primary accent (focus, active tab, links)
    uint32_t neonB;        // secondary accent (primary button, alerts)
    uint32_t text;
    uint32_t textDim;
    uint32_t border;
    uint32_t danger;       // close button hover
    uint32_t bars[4];      // bar colours cycled over tabs / sidebar blocks
};

constexpr int kChromeCount = static_cast<int>(ChromeStyle::Count);

const ChromeSpec& ChromeAt(int id);
const ChromeSpec& Chrome();            // the active style
int ChromeId();
void SetChrome(int id);                // clamps to the table

// The face the chrome letters in right now: the wanted face when it resolved
// (bundled or installed), else the skin's installed uiFont; nullptr for
// Classic. The app sets it after fonts load and whenever the skin changes.
const wchar_t* ChromeFace();
void SetChromeFace(const wchar_t* face);

// True for any skin that draws its own shapes (everything but Classic).
inline bool ChromeSkinned() { return !Chrome().useThemeAccent; }

// Hairline skins draw outlines instead of fills; this is the pen width in
// logical px, already 0 for every filled skin.
inline float ChromeOutline() { return Chrome().outline; }

} // namespace amber
