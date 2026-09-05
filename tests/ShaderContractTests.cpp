// ShaderContractTests.cpp — the CPU/GPU boundary. These catch the class of bug
// where a field is added to a C++ constant buffer and the HLSL mirror is
// forgotten, which produces silently wrong rendering rather than a compile error.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <cmath>

#include "common.h"
#include "render/motion_styles.h"
#include "render/desktop.h"
#include "ui/Chrome.h"

namespace
{

// Rec. 601 luma of a packed 0xRRGGBB colour, mirroring amber::LumaSrgb. Kept
// local so this file does not have to pull in Windows.h through Theme.h.
float Luma(uint32_t c)
{
    return (0.299f * ((c >> 16) & 0xFF) + 0.587f * ((c >> 8) & 0xFF) +
            0.114f * (c & 0xFF)) / 255.0f;
}

std::string ReadShader(const char* name)
{
#ifdef AMBER_SHADER_DIR
    std::string path = std::string(AMBER_SHADER_DIR) + "/" + name;
#else
    std::string path = std::string("shaders/") + name;
#endif
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Counts scalar slots declared in the cbuffer body: every comma-separated name
// on a `float`/`uint` line counts as one 4-byte slot.
size_t CountScalars(const std::string& body)
{
    size_t scalars = 0;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line))
    {
        auto hash = line.find("//");
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        if (line.find("float") == std::string::npos &&
            line.find("uint") == std::string::npos)
            continue;
        auto semi = line.find(';');
        if (semi == std::string::npos)
            continue;
        // Vector width: floatN/uintN/intN occupy N slots per name. A bare
        // float/uint is width 1.
        size_t width = 1;
        for (const char* base : { "float", "uint", "int" })
        {
            auto p = line.find(base);
            if (p == std::string::npos)
                continue;
            char c = line[p + std::strlen(base)];
            if (c >= '2' && c <= '4')
                width = static_cast<size_t>(c - '0');
            break;
        }
        // one name, plus one more per comma — each name is `width` slots
        size_t names = 1;
        for (char c : line.substr(0, semi))
            if (c == ',')
                ++names;
        scalars += names * width;
    }
    return scalars;
}

} // namespace

TEST_CASE("shader sources are present", "[shader][contract]")
{
    REQUIRE_FALSE(ReadShader("amber_common.hlsli").empty());
    REQUIRE_FALSE(ReadShader("particle_sim.hlsl").empty());
    REQUIRE_FALSE(ReadShader("particle_draw.hlsl").empty());
}

TEST_CASE("FrameCB C++ size matches its HLSL scalar count", "[shader][contract]")
{
    std::string src = ReadShader("amber_common.hlsli");
    REQUIRE_FALSE(src.empty());

    auto begin = src.find("cbuffer FrameCB");
    REQUIRE(begin != std::string::npos);
    auto open = src.find('{', begin);
    auto close = src.find("};", open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);

    std::string body = src.substr(open + 1, close - open - 1);
    size_t scalars = CountScalars(body);

    // Every scalar is 4 bytes and the struct is tightly packed on both sides.
    REQUIRE(scalars * 4 == sizeof(FrameCB));
}

TEST_CASE("FrameCB is 16-byte aligned for constant-buffer upload",
          "[shader][contract]")
{
    REQUIRE(sizeof(FrameCB) % 16 == 0);
}

TEST_CASE("CellGpu layout matches the HLSL struct", "[shader][contract]")
{
    REQUIRE(sizeof(CellGpu) == 24);

    std::string src = ReadShader("amber_common.hlsli");
    auto begin = src.find("struct CellGpu");
    REQUIRE(begin != std::string::npos);
    auto open = src.find('{', begin);
    auto close = src.find("};", open);
    std::string body = src.substr(open + 1, close - open - 1);
    REQUIRE(CountScalars(body) * 4 == sizeof(CellGpu));
}

TEST_CASE("particle template stride agrees between C++ and HLSL",
          "[shader][contract]")
{
    std::string src = ReadShader("amber_common.hlsli");
    auto pos = src.find("kMaxPPC");
    REQUIRE(pos != std::string::npos);
    auto eq = src.find('=', pos);
    auto semi = src.find(';', eq);
    int hlslStride = std::stoi(src.substr(eq + 1, semi - eq - 1));

    // The glyph sampler emits exactly this many points per glyph; densities
    // above it reuse points on other layers.
    REQUIRE(hlslStride == static_cast<int>(kParticlesPerCell));
}

TEST_CASE("the simulation shader declares the expected bindings",
          "[shader][contract]")
{
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE(src.find("RWStructuredBuffer<Particle>") != std::string::npos);
    REQUIRE(src.find("register(u0)") != std::string::npos);
    REQUIRE(src.find("StructuredBuffer<CellGpu>") != std::string::npos);
    REQUIRE(src.find("register(t0)") != std::string::npos);
    // Glyph templates: float4 per slot (xy position, z coverage weight),
    // mirrored by GlyphPoint on the C++ side.
    REQUIRE(src.find("StructuredBuffer<float4>") != std::string::npos);
    REQUIRE(src.find("register(t1)") != std::string::npos);
    REQUIRE(sizeof(GlyphPoint) == 16);
}

TEST_CASE("the simulation dispatches in groups of 256", "[shader][contract]")
{
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE(src.find("[numthreads(256, 1, 1)]") != std::string::npos);
}

TEST_CASE("the simulation clamps its timestep", "[shader][contract][stability]")
{
    // Without this clamp a paused debugger or a dragged window produces a huge
    // dt and the spring integrator explodes.
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE(src.find("min(dt,") != std::string::npos);
}

TEST_CASE("the four-layer model and force field are present",
          "[shader][contract][particles]")
{
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE(src.find("layer") != std::string::npos);
    REQUIRE(src.find("mouseForce") != std::string::npos);
    REQUIRE(src.find("mouseRadius") != std::string::npos);
    REQUIRE(src.find("shockTime") != std::string::npos);
    REQUIRE(src.find("animStyle") != std::string::npos);
}

TEST_CASE("quiet cells return before any motion-style code runs",
          "[shader][contract][particles]")
{
    // Stationary full-screen apps (htop) change cells in place many times a
    // second. The CPU marks those generations quiet (bit 2); the shader must
    // handle them in a self-contained block that RETURNS before the first
    // style-specific code, so no present or future motion style can reach
    // them. This test pins that structure.
    REQUIRE(kCellFlagQuiet == 4u);
    REQUIRE(kVisQuiet == 4u);
    REQUIRE((kCellFlagQuiet & (kCellFlagSelected | kCellFlagColor)) == 0u);
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE(src.find("(cd.flags & 4u)") != std::string::npos);

    REQUIRE(src.find("QUIET CELL") != std::string::npos);   // the banner
    // The block itself starts at its guard (the banner text is also quoted
    // by the declaration comment near the top of CSMain).
    const size_t quiet = src.find("if (quietC && cell != cursorIndex)");
    REQUIRE(quiet != std::string::npos);
    const size_t ret = src.find("return;", quiet);
    REQUIRE(ret != std::string::npos);
    // The quiet block never consults the global style...
    REQUIRE(src.substr(quiet, ret - quiet).find("animStyle") == std::string::npos);
    // ...and returns before every style-specific per-cell section: the
    // brightness/home tweaks, the cursor costumes, the scatter burst and
    // the style integrators.
    const size_t firstStyle = src.find("Quantum Flux transit character");
    const size_t cursorStyle = src.find("Cursor cell: each motion style");
    const size_t scatter = src.find("Nebula Twist: 45-80 px collapse");
    const size_t forces = src.find("// NEBULA TWIST");
    REQUIRE(firstStyle != std::string::npos);
    REQUIRE(cursorStyle != std::string::npos);
    REQUIRE(scatter != std::string::npos);
    REQUIRE(forces != std::string::npos);
    REQUIRE(ret < firstStyle);
    REQUIRE(ret < cursorStyle);
    REQUIRE(ret < scatter);
    REQUIRE(ret < forces);
    // No per-cell style-id substitution remains: the early return IS the
    // mechanism, so a new style written against animStyle stays safe.
    REQUIRE(src.find("styleC") == std::string::npos);
}

TEST_CASE("every registered motion style is implemented in the shader",
          "[shader][contract][particles]")
{
    // The CPU registry and the GPU switch chain are two halves of one feature.
    // Adding a row to kMotionStyles without writing its behaviour would give a
    // menu entry that silently animates like Direct, so pin the pairing.
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE_FALSE(src.empty());
    for (int i = 1; i < kMotionStyleCount; ++i)
    {
        INFO("motion style " << i << " = " << kMotionStyles[i].name);
        REQUIRE(src.find("animStyle == " + std::to_string(i) + "u") !=
                std::string::npos);
    }
    // Style 0 is Direct: it is the shader's default path, not a case.
    REQUIRE(std::string(kMotionStyles[0].name) == "Direct");
}

TEST_CASE("the motion style registry is well formed",
          "[contract][particles]")
{
    REQUIRE(kMotionStyleCount >= 12);
    std::map<wchar_t, int> accelUse;
    for (int i = 0; i < kMotionStyleCount; ++i)
    {
        const MotionStyleInfo& s = kMotionStyles[i];
        INFO("motion style " << i << " = " << s.name);
        REQUIRE(s.name != nullptr);
        REQUIRE(std::strlen(s.name) > 0);
        REQUIRE(s.menu != nullptr);
        // Menu accelerators must be unique inside the popup.
        std::wstring menu(s.menu);
        size_t amp = menu.find(L'&');
        REQUIRE(amp != std::wstring::npos);
        REQUIRE(amp + 1 < menu.size());
        wchar_t key = static_cast<wchar_t>(::towlower(menu[amp + 1]));
        // Uniqueness is no longer reachable: past about twenty styles the
        // letters that actually occur in the names run out. Windows handles a
        // repeated accelerator by cycling between the matching items, which is
        // its documented behaviour, so the test bounds the collisions instead
        // of forbidding them.
        ++accelUse[key];
        INFO("accelerator '" << static_cast<char>(key) << "' used "
                             << accelUse[key] << " times");
        REQUIRE(accelUse[key] <= 2);
        // A hold longer than the effect itself would strand the sharp letter.
        REQUIRE(s.coreDelay >= 0.0f);
        REQUIRE(s.coreDelay <= 2.0f);
        REQUIRE(s.waveScale >= 1.0f);
        REQUIRE(s.waveScale <= 4.0f);
    }
    // Out-of-range indices (an older settings file, a downgraded build) fall
    // back to Direct rather than reading past the table.
    REQUIRE(std::string(MotionStyleAt(9999u).name) == "Direct");
    REQUIRE(std::string(MotionStyleAt(static_cast<uint32_t>(kMotionStyleCount)).name)
            == "Direct");
}

TEST_CASE("reduced motion is applied on the CPU, not per style in the shader",
          "[shader][contract][particles][a11y]")
{
    // Reduced motion works by uploading Direct instead of the selected style,
    // so every style — including ones added later — inherits it for free. If
    // the shader ever grew its own reduced-motion branch, a new style could
    // forget to honour it. Keep the mechanism where it cannot be forgotten.
    std::string src = ReadShader("particle_sim.hlsl");
    REQUIRE(src.find("reducedMotion") == std::string::npos);
}

TEST_CASE("every interface skin is well formed", "[contract][chrome]")
{
    // A skin is pure data, so the things that can go wrong are data mistakes:
    // a missing name, an unreadable text-on-ground pairing, a pill skin with
    // no bar colours to cycle, or a light skin that forgot to say so.
    REQUIRE(amber::kChromeCount >= 8);
    std::set<std::string> names;
    for (int i = 0; i < amber::kChromeCount; ++i)
    {
        const amber::ChromeSpec& ch = amber::ChromeAt(i);
        INFO("skin " << i << " = " << (ch.name ? ch.name : "(null)"));
        REQUIRE(ch.name != nullptr);
        REQUIRE(std::strlen(ch.name) > 0);
        REQUIRE(names.insert(ch.name).second);       // names are unique
        if (ch.useThemeAccent)
            continue;                                // Classic borrows the theme
        // Body text must actually be readable on the skin's own ground.
        float ground = Luma(ch.bg);
        float ink = Luma(ch.text);
        INFO("ground luma " << ground << " vs text luma " << ink);
        REQUIRE(std::fabs(ground - ink) > 0.25f);
        // lightGround must agree with the ground it actually declares.
        REQUIRE(ch.lightGround == (ground > 0.55f));
        // Pill skins cycle bars[]; they must not all be the ground colour.
        if (ch.pills)
        {
            bool any = false;
            for (uint32_t b : ch.bars)
                any = any || (b != ch.bg);
            REQUIRE(any);
        }
        REQUIRE(ch.outline >= 0.0f);
        REQUIRE(ch.outline < 6.0f);
    }
    // Out-of-range ids clamp rather than reading past the table.
    REQUIRE(std::string(amber::ChromeAt(-4).name) ==
            std::string(amber::ChromeAt(0).name));
    REQUIRE(std::string(amber::ChromeAt(9999).name) ==
            std::string(amber::ChromeAt(amber::kChromeCount - 1).name));
}

TEST_CASE("scene format is an HDR float target", "[shader][contract]")
{
    REQUIRE(kSceneFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
}

TEST_CASE("frames in flight is a sane triple-buffer value", "[shader][contract]")
{
    REQUIRE(kFramesInFlight >= 2);
    REQUIRE(kFramesInFlight <= 4);
}

// ---- the VNC particle desktop --------------------------------------------------
// DesktopCB lives in shaders/desktop_common.hlsli at register b1 and is
// mirrored by DesktopCB in src/render/desktop.h. Same rule as FrameCB: the
// HLSL body's scalar count times four must equal the C++ size.
TEST_CASE("DesktopCB C++ size matches its HLSL scalar count", "[shader][contract][vnc]")
{
    const std::string src = ReadShader("desktop_common.hlsli");
    REQUIRE_FALSE(src.empty());
    const size_t begin = src.find("cbuffer DesktopCB");
    REQUIRE(begin != std::string::npos);
    const size_t open = src.find('{', begin);
    const size_t close = src.find("};", open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    const std::string body = src.substr(open + 1, close - open - 1);
    const size_t scalars = CountScalars(body);
    REQUIRE(scalars * 4 == sizeof(DesktopCB));
    REQUIRE(sizeof(DesktopCB) % 16 == 0);
}

TEST_CASE("the desktop shaders are present and include the shared contract", "[shader][contract][vnc]")
{
    for (const char* name : { "desktop_common.hlsli", "motion_fields.hlsli", "desktop_energy.hlsl",
                              "desktop_sim.hlsl", "desktop_draw.hlsl" })
        REQUIRE_FALSE(ReadShader(name).empty());
    // every motion style in kMotionStyles has a field: the switch covers 0..count-1
    const std::string fields = ReadShader("motion_fields.hlsli");
    for (int i = 0; i < kMotionStyleCount; ++i)
        REQUIRE(fields.find("case " + std::to_string(i) + ":") != std::string::npos);
}
