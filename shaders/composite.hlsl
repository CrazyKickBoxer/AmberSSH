// composite.hlsl — bloom add, vignette, scanlines, ACES tonemap (SDR) or
// scRGB pass-through with a soft shoulder at display peak (HDR10/scRGB).
cbuffer CompositeCB : register(b0)
{
    float bloomStrength;
    float vignetteAmt;      // max darkening at extreme corners
    float scanAmp;          // +/- brightness modulation, 2px period
    float timeSec;
    uint  hdrMode;          // 0 = SDR (gamma out), 1 = scRGB linear out
    float maxNits;          // display peak from DXGI_OUTPUT_DESC1
    float paperWhiteNits;   // 1.0 scene units map to this many nits in HDR
    float exposure;
    float grayscale;        // 1 = desaturate to luminance (e-ink / Hercules)
    float pixelBlock;       // >0 = pixel-art block size in device px (0 = off)
    float invResX, invResY; // 1 / backbuffer size, for block→uv snapping
    float flashR, flashG, flashB, flashAmt;  // exit-code edge flash (additive)
    float crtOff;           // 0..1 CRT power-off collapse on tab close
    float shakeX, shakeY;   // screen-shake sample offset (uv units)
    // Compiz cube on session switch: rotation 0..pi/2 (0 = off), direction
    // (+1 = the next session rises from the right, -1 = from the left),
    // backbuffer aspect, and the colour of the void behind the cube.
    float cubeAngle, cubeDir, aspect;
    float cubeBgR, cubeBgG, cubeBgB;
};

Texture2D<float4> gScene : register(t0);
Texture2D<float4> gBloom : register(t1);
Texture2D<float4> gSnap      : register(t2);   // departing frame (scene)
Texture2D<float4> gSnapBloom : register(t3);   // departing frame (bloom)
SamplerState      gLin   : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 t = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(t.x, 1.0 - t.y);
    return o;
}

float3 ACESFilm(float3 x)
{
    // Narkowicz ACES approximation.
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

// PICO-8 palette — a punchy, cartoony 16-colour set (the "movie hacker /
// arcade" look) that stays readable when the terminal quantizes to it.
static const float3 kPico8[16] = {
    float3(0.000, 0.000, 0.000), float3(0.114, 0.169, 0.325),
    float3(0.494, 0.145, 0.325), float3(0.000, 0.529, 0.318),
    float3(0.671, 0.322, 0.212), float3(0.373, 0.341, 0.310),
    float3(0.761, 0.765, 0.780), float3(1.000, 0.945, 0.910),
    float3(1.000, 0.000, 0.302), float3(1.000, 0.639, 0.000),
    float3(1.000, 0.925, 0.153), float3(0.000, 0.894, 0.212),
    float3(0.161, 0.678, 1.000), float3(0.514, 0.463, 0.612),
    float3(1.000, 0.467, 0.659), float3(1.000, 0.800, 0.667),
};

// Compiz-style cube. The cube is unit-sized (faces at distance 1 from its
// centre, half-height 1/aspect) and the camera sits on +z at a distance
// that makes the front face fill the screen exactly at angle 0. Rotating
// the cube by -dir*phi brings the side face at x = dir to the front; we
// rotate the pixel ray into cube space instead and intersect two planes:
//   front face  z = +1  -> the departing session (frozen snapshot)
//   side face   x = dir -> the arriving session (live scene)
// Mid-turn the camera pulls back a little so both faces fit, and faces
// are shaded by how squarely they face the viewer (Compiz's cube lighting).
// At phi = pi/2 the side face maps 1:1 to the screen, so the turn ends
// seamlessly on the live scene.
bool CubeRemap(float2 uv, out float2 faceUv, out float useSnap, out float shade)
{
    faceUv = uv;
    useSnap = 0.0;
    shade = 1.0;
    float phi = cubeAngle;
    float s = (cubeDir < 0.0) ? -1.0 : 1.0;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float d = 1.8;                          // camera distance to the front face
    float pull = 0.8 * sin(2.0 * phi);      // zero at both ends of the turn
    float3 o = float3(0.0, 0.0, 1.0 + d + pull);
    float3 dir = normalize(float3(ndc.x / d, ndc.y / (d * aspect), -1.0));
    float a = s * phi;
    float ca = cos(a), sa = sin(a);
    float3 oc = float3(o.x * ca + o.z * sa, o.y, -o.x * sa + o.z * ca);
    float3 dc = float3(dir.x * ca + dir.z * sa, dir.y, -dir.x * sa + dir.z * ca);
    float hy = 1.0 / aspect;
    float bestT = 1e9;
    bool hit = false;
    if (dc.z < 0.0)                         // front face, z = +1
    {
        float t = (1.0 - oc.z) / dc.z;
        float3 p = oc + dc * t;
        if (t > 0.0 && abs(p.x) <= 1.0 && abs(p.y) <= hy)
        {
            bestT = t;
            faceUv = float2((p.x + 1.0) * 0.5, (1.0 - p.y * aspect) * 0.5);
            useSnap = 1.0;
            shade = 0.45 + 0.55 * cos(phi);
            hit = true;
        }
    }
    if (dc.x * s < 0.0)                     // side face, x = s
    {
        float t = (s - oc.x) / dc.x;
        float3 p = oc + dc * t;
        if (t > 0.0 && t < bestT && abs(p.z) <= 1.0 && abs(p.y) <= hy)
        {
            faceUv = float2((1.0 - s * p.z) * 0.5, (1.0 - p.y * aspect) * 0.5);
            useSnap = 0.0;
            shade = 0.45 + 0.55 * sin(phi);
            hit = true;
        }
    }
    return hit;
}

float4 PSMain(VSOut i) : SV_Target
{
    // Pixel-art mode: snap the sample to chunky blocks (nearest-neighbour).
    float2 uv = i.uv;
    if (pixelBlock > 0.5)
    {
        float2 px = floor(i.pos.xy / pixelBlock) * pixelBlock + pixelBlock * 0.5;
        uv = px * float2(invResX, invResY);
    }
    // CRT power-off: the raster squeezes to a bright horizontal line, then
    // the line pinches to a dot and dies — the classic tube collapse.
    float crtMask = 1.0;
    float3 crtGlow = float3(0.0, 0.0, 0.0);
    if (crtOff > 0.001)
    {
        float t = saturate(crtOff);
        float sv = max(1.0 - t * 1.18, 0.004);                 // vertical squash
        float sh = (t > 0.86) ? max(1.0 - (t - 0.86) / 0.12, 0.02)
                              : 1.0;                            // then horizontal
        float2 cv = i.uv - 0.5;
        float2 wuv = float2(cv.x / sh, cv.y / sv) + 0.5;
        bool inside = wuv.x >= 0.0 && wuv.x <= 1.0 &&
                      wuv.y >= 0.0 && wuv.y <= 1.0;
        // The gun's dying beam: a hot amber band along the collapse axis.
        float band = exp(-pow(cv.y / (sv * 0.55 + 1e-3), 2.0)) *
                     exp(-pow(cv.x / (sh * 0.55 + 1e-3), 2.0));
        crtGlow = float3(1.0, 0.72, 0.28) * t * t * band * 2.2;
        uv = inside ? wuv : uv;
        crtMask = inside ? (1.0 + t * 2.0) : 0.0;              // brighten, clip
    }

    // Screen shake (OOM / segfault / kill): the whole raster jolts.
    uv += float2(shakeX, shakeY);

    float3 c;
    if (cubeAngle > 0.0005)
    {
        float2 fuv;
        float useSnap, shade;
        if (CubeRemap(uv, fuv, useSnap, shade))
        {
            float3 live = gScene.Sample(gLin, fuv).rgb +
                          gBloom.Sample(gLin, fuv).rgb * bloomStrength;
            float3 snap = gSnap.Sample(gLin, fuv).rgb +
                          gSnapBloom.Sample(gLin, fuv).rgb * bloomStrength;
            c = lerp(live, snap, useSnap) * shade;
        }
        else
            c = float3(cubeBgR, cubeBgG, cubeBgB);   // the void behind the cube
    }
    else
    {
        c = gScene.Sample(gLin, uv).rgb;
        c += gBloom.Sample(gLin, uv).rgb * bloomStrength;
    }
    c *= exposure;
    c = c * crtMask + crtGlow;

    // Exit-code flash: a brief tinted glow breathing in from the screen
    // edges — green whisper on success, red ember wash on failure.
    if (flashAmt > 0.001)
    {
        float2 fq = i.uv * 2.0 - 1.0;
        float edge = pow(saturate(dot(fq, fq) * 0.55), 1.3);
        c += float3(flashR, flashG, flashB) * flashAmt * edge;
    }

    // E-ink: collapse to luminance, then a warm amber cast so the page reads
    // like the original Kindle warm light (soft yellow, easy on the eyes).
    if (grayscale > 0.5)
        c = dot(c, float3(0.2126, 0.7152, 0.0722)) * float3(1.07, 0.99, 0.72);

    // Faint scanlines: 2px period, barely visible.
    c *= 1.0 + scanAmp * sin(i.pos.y * 3.14159265);

    // Edge vignette.
    float2 q = i.uv * 2.0 - 1.0;
    float  v = pow(saturate(dot(q, q) * 0.5), 1.6);
    c *= 1.0 - vignetteAmt * v;

    if (hdrMode != 0)
    {
        // scRGB: linear, 1.0 == 80 nits. Scale so scene 1.0 hits paper white,
        // then a soft Reinhard-style shoulder at the display's peak.
        float scale = paperWhiteNits / 80.0;
        float peak  = max(maxNits, 400.0) / 80.0;
        c *= scale;
        float lum = max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-5);
        float mapped = lum * (1.0 + lum / (peak * peak)) / (1.0 + lum);
        c *= mapped / lum;
        return float4(c, 1.0);
    }
    else
    {
        // SDR: in-range colors pass through untouched so ANSI/truecolor stays
        // exact; the filmic curve only takes over where particle/bloom energy
        // exceeds the displayable range.
        float m = max(c.r, max(c.g, c.b));
        float3 filmic = ACESFilm(c);
        c = lerp(c, filmic, smoothstep(0.75, 1.35, m));
        c = saturate(c);
        c = pow(c, 1.0 / 2.2);
        // Snap to the 16-colour palette (in display/sRGB space) for the
        // cartoony pixel-art look.
        if (pixelBlock > 0.5)
        {
            float best = 1e9;
            float3 pick = c;
            [unroll]
            for (int p = 0; p < 16; ++p)
            {
                float3 d = c - kPico8[p];
                float dd = dot(d, d);
                if (dd < best) { best = dd; pick = kPico8[p]; }
            }
            c = pick;
        }
        return float4(c, 1.0);
    }
}
