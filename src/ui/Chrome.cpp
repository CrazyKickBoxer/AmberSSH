#include "Chrome.h"

#include <algorithm>
#include <string>

namespace amber
{

namespace
{

const ChromeSpec kChromes[kChromeCount] = {
    // Cyberpunk: near-black ground, cyan + magenta neon, leaning tabs,
    // chamfered corners, HUD bracket ticks and faint scanlines.
    {
        "Cyberpunk",
        /*tabSlant*/ 9.0f, /*chamfer*/ 7.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ false, /*hudBrackets*/ true,
        /*scanlines*/ true, /*numberedTabs*/ true, /*glow*/ true,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ false, /*uiFont*/ nullptr, /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x07080D, /*panel*/ 0x0D1119, /*field*/ 0x0A0E15,
        /*neonA*/ 0x00E5FF, /*neonB*/ 0xFF2BD6,
        /*text*/ 0xD8F6FF, /*textDim*/ 0x5FA8B8, /*border*/ 0x1F4A55,
        /*danger*/ 0xFF3B5C,
        { 0x0D1119, 0x0D1119, 0x0D1119, 0x0D1119 },
    },
    // Classic: the original Termius-style chrome coloured by the theme.
    {
        "Classic",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ false, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ false, /*uiFont*/ nullptr, /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ true,
        0x000000, 0x000000, 0x000000, 0xFFB000, 0xFFB000, 0xFFD080, 0xC08040,
        0x604020, 0xE0403A,
        { 0, 0, 0, 0 },
    },
    // LCARS: the Okudagram standard. Black ground; Melrose, Golden Tanoi,
    // Chestnut Rose and Lilac cycled over the sidebar blocks; Orange Peel for
    // the active element and Atomic Tangerine for body text. Ultra-condensed
    // capitals in black on every colour block. Colour names and hexes are the
    // published LCARS colour standard — see the LCARS Reference board.
    {
        "LCARS",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ true, /*elbow*/ true,
        /*darkText*/ true, /*condensedFont*/ true, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift Condensed", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x000000, /*panel*/ 0x9999FF, /*field*/ 0x000000,
        /*neonA*/ 0xFF9900, /*neonB*/ 0xFFCC66,
        /*text*/ 0xFF9966, /*textDim*/ 0x9999CC, /*border*/ 0x664466,
        /*danger*/ 0xCC6666,
        { 0x9999FF, 0xFFCC66, 0xCC6666, 0xCC99CC },
    },
    // Nostromo: the 1979 ship-computer console. Green phosphor on a near-black
    // ground with a green cast, everything square, heavy scanlines, corner
    // brackets, capitals throughout, and an amber alarm colour that is the only
    // thing on screen that is not green.
    {
        "Nostromo",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ false, /*hudBrackets*/ true,
        /*scanlines*/ true, /*numberedTabs*/ true, /*glow*/ true,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Consolas", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x02090A, /*panel*/ 0x061613, /*field*/ 0x03100E,
        /*neonA*/ 0x33FF88, /*neonB*/ 0xB6FFCE,
        /*text*/ 0x9BFFC4, /*textDim*/ 0x3E8F63, /*border*/ 0x1B5B3C,
        /*danger*/ 0xFF6B3D,
        { 0x061613, 0x061613, 0x061613, 0x061613 },
    },
    // Blueprint: a cyanotype drafting sheet. Nothing is filled — every panel,
    // tab and field is a hairline outline in white or cyan ink on deep blue,
    // with drafting ticks at the corners and a title block for the wordmark.
    {
        "Blueprint",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ true, /*hudBrackets*/ true,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 1.4f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift Condensed", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x0B2545, /*panel*/ 0x102F55, /*field*/ 0x0C2A4C,
        /*neonA*/ 0xE8F1FF, /*neonB*/ 0x7FC4FF,
        /*text*/ 0xDCE9FA, /*textDim*/ 0x7FA3C9, /*border*/ 0x3D6893,
        /*danger*/ 0xFF8A7A,
        { 0x14395F, 0x1B4670, 0x14395F, 0x1B4670 },
    },
    // Brass and Nixie: a warm instrument panel. Dark walnut ground, brushed
    // brass segments with black engraving, amber nixie digits, rounded bezels.
    // Reuses the LCARS pill geometry with an entirely different temperature.
    {
        "Brass & Nixie",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ true, /*elbow*/ false,
        /*darkText*/ true, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ true,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ false, /*uiFont*/ L"Georgia", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x140F0A, /*panel*/ 0xB08D57, /*field*/ 0x1C140C,
        /*neonA*/ 0xFFB347, /*neonB*/ 0xFFD9A0,
        /*text*/ 0xE8C79A, /*textDim*/ 0x8C6A45, /*border*/ 0x6E5334,
        /*danger*/ 0xC0392B,
        { 0xB08D57, 0xC9A227, 0x8C6239, 0xD9B36C },
    },
    // Swiss Grid: the one light chrome. Paper ground, black type, a single
    // saturated red, hard right angles and no ornament at all. Note that this
    // skins the WINDOW, not the terminal — pair it with the Paperwhite or
    // Light appearance mode for a fully light application.
    {
        "Swiss Grid",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ true, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ true, /*hazard*/ false, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Arial", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0xF2F2F0, /*panel*/ 0xFFFFFF, /*field*/ 0xFFFFFF,
        /*neonA*/ 0xE2231A, /*neonB*/ 0x111111,
        /*text*/ 0x111111, /*textDim*/ 0x6B6B6B, /*border*/ 0xCFCFCB,
        /*danger*/ 0xE2231A,
        { 0x111111, 0xE2231A, 0x9A9A96, 0xFFFFFF },
    },
    // Neo-Tokyo: red on black with hard forward slants, capitals, scanlines
    // and yellow-black hazard striping wherever something is destructive.
    {
        "Neo-Tokyo",
        /*tabSlant*/ 14.0f, /*chamfer*/ 5.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ true, /*hudBrackets*/ false,
        /*scanlines*/ true, /*numberedTabs*/ true, /*glow*/ true,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ true, /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift Condensed", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x0A0507, /*panel*/ 0x18090C, /*field*/ 0x120608,
        /*neonA*/ 0xFF2D3E, /*neonB*/ 0xFFD400,
        /*text*/ 0xFFE3E6, /*textDim*/ 0xA05560, /*border*/ 0x5E1620,
        /*danger*/ 0xFFD400,
        { 0x18090C, 0x2A0A10, 0x18090C, 0x2A0A10 },
    },
    // Steampunk: the Figma board rendered as chrome. Coal and walnut ground,
    // brass trim, gaslight for the accent, oxblood for alarm, parchment ink.
    // Chamfered corners and HUD ticks read as machined plates and rivets.
    // Distinct from Brass & Nixie, which is polished pills with black
    // engraving; this one is darker, oxidised, and cut rather than moulded.
    {
        "Steampunk",
        /*tabSlant*/ 0.0f, /*chamfer*/ 6.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ false, /*hudBrackets*/ true,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ true,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false, /*ornament*/ true, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ false, /*uiFont*/ L"Bookman Old Style", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x17120D, /*panel*/ 0x2A1E14, /*field*/ 0x1F160E,
        /*neonA*/ 0xB08D57, /*neonB*/ 0xFFA53C,
        /*text*/ 0xEDE0C8, /*textDim*/ 0xA2917A, /*border*/ 0x4A3524,
        /*danger*/ 0x7C2B22,
        { 0xB08D57, 0xA45A2A, 0x4E8577, 0xE3C27E },
    },
    // Solder Mask: a populated board. Matte mask green ground, ENIG gold for
    // anything that carries signal (focus, the active tab, primary actions),
    // bare copper for everything else, HASL tin for junctions the user can
    // act on, and silkscreen white capitals. Dividers are not lines — they
    // are routed traces with 45 degree mitres and vias. Polarity red marks
    // destructive controls and nothing else. See the Figma Solder Mask board.
    {
        "Solder Mask",
        /*tabSlant*/ 0.0f, /*chamfer*/ 5.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ true, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false,
        /*ornament*/ false, /*traces*/ true, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0x0B3A24, /*panel*/ 0x114A2E, /*field*/ 0x082B1B,
        /*neonA*/ 0xE3B23C, /*neonB*/ 0xF2C75C,
        /*text*/ 0xEAF2EC, /*textDim*/ 0x8FA898, /*border*/ 0x1D6B43,
        /*danger*/ 0xC0392B,
        // Dulled gold fingers, the way an edge connector actually looks; the
        // active tab and the primary action are the only bright gold on the
        // board. bars[1] is the hover fill, so it lifts off bars[0].
        { 0x9A7628, 0xC69A34, 0xA9832C, 0xC87137 },
    },
    // Horologe: haute horlogerie. A sunburst anthracite dial, rhodium
    // lettering, lume for the active piece and the selection, and exactly
    // one blued screw at rest — the close cap. Controls are applied indices:
    // raised by a rhodium line above and a shade below, never filled. Every
    // long edge carries the rehaut minute track. See the Figma Horologe board.
    {
        "Horologe",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ true, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false,
        /*ornament*/ false, /*traces*/ false, /*rehaut*/ true,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift Light", /*uiFontWanted*/ L"Jost",
        /*useThemeAccent*/ false,
        /*bg*/ 0x23262B, /*panel*/ 0x2C3037, /*field*/ 0x1A1D21,
        // neonA is lume (focus, active tab, selection, banner); neonB is the
        // blued screw (primary action, close cap, the hovered piece).
        /*neonA*/ 0xD8E6C4, /*neonB*/ 0x2857C9,
        /*text*/ 0xC9CCD0, /*textDim*/ 0x7E848C, /*border*/ 0x3A3E45,
        /*danger*/ 0xB3202A,
        // The dial ring for every index; bars[1] is the hover lift.
        { 0x2C3037, 0x383C44, 0x2C3037, 0x2C3037 },
    },
    // Letterpress: a one-ink press on cotton stock. Everything printed is the
    // same oxblood; the only exception is copper foil, reserved for the active
    // tab and the primary action. Inactive controls are blind-debossed — the
    // impression with no ink at all. The second light skin after Swiss.
    {
        "Letterpress",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ true, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ true, /*hazard*/ false,
        /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ true, /*stitch*/ false, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ false, /*uiFont*/ L"Georgia", /*uiFontWanted*/ L"EB Garamond",
        /*useThemeAccent*/ false,
        /*bg*/ 0xF2F1EC, /*panel*/ 0xEAE8E1, /*field*/ 0xE4E1D9,
        /*neonA*/ 0x6E1B2A, /*neonB*/ 0xB87333,
        /*text*/ 0x6E1B2A, /*textDim*/ 0xB58A92, /*border*/ 0xCBC7BC,
        /*danger*/ 0x4A0F1B,
        { 0xF2F1EC, 0xE4E1D9, 0xCBC7BC, 0xB87333 },
    },
    // Atelier: full-grain oxblood hide. Outlines are saddle-stitched in cream
    // thread inside a painted edge; the active piece is fixed with a brass
    // rivet rather than recoloured; fields are lined with a charcoal check.
    // Brass appears as dots only — a snap, a rivet — never a bar.
    {
        "Atelier",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false,
        /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ true, /*meter*/ false, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift Light", /*uiFontWanted*/ L"Josefin Sans",
        /*useThemeAccent*/ false,
        /*bg*/ 0x3E1B1F, /*panel*/ 0x4C2328, /*field*/ 0x26222A,
        /*neonA*/ 0xE9DCC3, /*neonB*/ 0xB08D57,
        /*text*/ 0xE9DCC3, /*textDim*/ 0xB9A88E, /*border*/ 0x24100F,
        /*danger*/ 0xC8443C,
        { 0x4C2328, 0x5A2A30, 0x4C2328, 0xB08D57 },
    },
    // Reference: boutique hi-fi. A brushed aluminium faceplate with engraved
    // lettering, source-selector buttons with an LED — the app's own amber —
    // and anything that is a level shown on a blue-backlit VU meter. The
    // primary action is a blue backlit pushbutton.
    {
        "Reference",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ true, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ true, /*hazard*/ false,
        /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ true, /*glaze*/ false,
        /*uppercase*/ true, /*uiFont*/ L"Bahnschrift SemiBold", /*uiFontWanted*/ nullptr,
        /*useThemeAccent*/ false,
        /*bg*/ 0xB4B7BB, /*panel*/ 0xC3C6CA, /*field*/ 0xE6E8EB,
        /*neonA*/ 0xFFB000, /*neonB*/ 0x1E4FC4,
        /*text*/ 0x2B2E33, /*textDim*/ 0x5E6267, /*border*/ 0x8E9195,
        /*danger*/ 0xF03A2E,
        { 0xA9ACB0, 0xC9CCD0, 0xA9ACB0, 0xA9ACB0 },
    },
    // Tenmoku: studio pottery. A hare's-fur iron glaze that pools darker at
    // every edge and thins to rust along the top, silver-blue oil-spot flecks,
    // a raw stoneware foot under every panel, and one cinnabar chop marking
    // the active piece. Nothing else on the surface is red.
    {
        "Tenmoku",
        /*tabSlant*/ 0.0f, /*chamfer*/ 0.0f, /*pills*/ false, /*elbow*/ false,
        /*darkText*/ false, /*condensedFont*/ false, /*hudBrackets*/ false,
        /*scanlines*/ false, /*numberedTabs*/ true, /*glow*/ false,
        /*outline*/ 0.0f, /*lightGround*/ false, /*hazard*/ false,
        /*ornament*/ false, /*traces*/ false, /*rehaut*/ false,
        /*impression*/ false, /*stitch*/ false, /*meter*/ false, /*glaze*/ true,
        /*uppercase*/ false, /*uiFont*/ L"Georgia", /*uiFontWanted*/ L"Alegreya",
        /*useThemeAccent*/ false,
        /*bg*/ 0x1E1614, /*panel*/ 0x2A1F1B, /*field*/ 0x17110F,
        /*neonA*/ 0x7FA0B8, /*neonB*/ 0x8B7355,
        /*text*/ 0xEDE4D6, /*textDim*/ 0x9A8C7C, /*border*/ 0x3A2C26,
        /*danger*/ 0xC23B22,
        { 0x2A1F1B, 0x33261F, 0x2A1F1B, 0x8B7355 },
    },
};

int gChromeId = 0;

} // namespace

const ChromeSpec& ChromeAt(int id)
{
    return kChromes[std::clamp(id, 0, kChromeCount - 1)];
}

const ChromeSpec& Chrome() { return ChromeAt(gChromeId); }

int ChromeId() { return gChromeId; }

void SetChrome(int id) { gChromeId = std::clamp(id, 0, kChromeCount - 1); }

} // namespace amber

namespace amber
{
namespace
{
std::wstring gChromeFace;
bool gChromeFaceSet = false;
} // namespace

const wchar_t* ChromeFace()
{
    const ChromeSpec& ch = Chrome();
    if (ch.useThemeAccent)
        return nullptr;
    if (gChromeFaceSet && !gChromeFace.empty())
        return gChromeFace.c_str();
    return ch.uiFont;
}

void SetChromeFace(const wchar_t* face)
{
    gChromeFaceSet = face != nullptr;
    gChromeFace = face ? face : L"";
}
} // namespace amber
