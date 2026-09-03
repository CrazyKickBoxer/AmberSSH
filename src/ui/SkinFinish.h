// SkinFinish.h — the Direct2D finish pass over a GDI-painted surface.
//
// GDI gives the skins hard 1px lines and flat fills. This is the layer that
// gives them what the pitch mocks had: gradients, soft inner shadows, glows,
// and anti-aliased curves — drawn OVER the GDI paint through a DC render
// target bound to the same HDC, so nothing about the Win32 structure of the
// dialogs changes. A pass is scoped: construct it, draw, let it go.
//
// Coordinates are the caller's GDI coordinates for the bound rect; the pass
// subtracts the rect origin itself, so a button paints in its own item space
// and a dialog in client space without either thinking about it.
#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <cmath>

#pragma comment(lib, "d2d1.lib")

namespace amber
{
namespace finish
{

using Microsoft::WRL::ComPtr;

inline ID2D1Factory* Factory()
{
    static ComPtr<ID2D1Factory> f;
    if (!f)
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                          reinterpret_cast<void**>(f.GetAddressOf()));
    return f.Get();
}

inline D2D1_COLOR_F Col(COLORREF c, float a = 1.0f)
{
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                        GetBValue(c) / 255.0f, a);
}

class Pass
{
public:
    Pass(HDC dc, const RECT& rc)
        : m_ox(static_cast<float>(rc.left)), m_oy(static_cast<float>(rc.top))
    {
        ID2D1Factory* f = Factory();
        if (!f)
            return;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(f->CreateDCRenderTarget(&props, m_rt.GetAddressOf())))
            return;
        RECT bind = rc;
        if (FAILED(m_rt->BindDC(dc, &bind)))
        {
            m_rt.Reset();
            return;
        }
        m_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_rt->BeginDraw();
        m_ok = true;
    }
    ~Pass()
    {
        if (m_ok)
            m_rt->EndDraw();
    }
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;
    bool ok() const { return m_ok; }

    // A two-stop linear gradient across the rect, top→bottom or left→right.
    void Gradient(const RECT& r, COLORREF c0, float a0, COLORREF c1, float a1,
                  bool vertical = true)
    {
        if (!m_ok) return;
        D2D1_GRADIENT_STOP st[2] = { { 0.0f, Col(c0, a0) }, { 1.0f, Col(c1, a1) } };
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(m_rt->CreateGradientStopCollection(st, 2, coll.GetAddressOf())))
            return;
        const D2D1_RECT_F rf = R(r);
        ComPtr<ID2D1LinearGradientBrush> b;
        m_rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(rf.left, rf.top),
                vertical ? D2D1::Point2F(rf.left, rf.bottom) : D2D1::Point2F(rf.right, rf.top)),
            coll.Get(), b.GetAddressOf());
        if (b)
            m_rt->FillRectangle(rf, b.Get());
    }

    // A three-stop gradient: the metallic case — dark, bright band, dark.
    void Metal(const RECT& r, COLORREF dark, COLORREF bright, float band, float alpha)
    {
        if (!m_ok) return;
        D2D1_GRADIENT_STOP st[3] = { { 0.0f, Col(dark, alpha) },
                                     { band, Col(bright, alpha) },
                                     { 1.0f, Col(dark, alpha) } };
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(m_rt->CreateGradientStopCollection(st, 3, coll.GetAddressOf())))
            return;
        const D2D1_RECT_F rf = R(r);
        ComPtr<ID2D1LinearGradientBrush> b;
        m_rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rf.left, rf.top),
                                                D2D1::Point2F(rf.right, rf.bottom)),
            coll.Get(), b.GetAddressOf());
        if (b)
            m_rt->FillRectangle(rf, b.Get());
    }

    // A specular sheen: white fading out over the top part of the rect.
    void Sheen(const RECT& r, float alpha, float extent = 0.55f)
    {
        if (!m_ok) return;
        RECT top = r;
        top.bottom = r.top + static_cast<LONG>((r.bottom - r.top) * extent);
        Gradient(top, RGB(255, 255, 255), alpha, RGB(255, 255, 255), 0.0f, true);
    }

    // A soft inner shadow: dark fading inward from every edge. Letterpress's
    // impression, Tenmoku's pooling, the well of any field.
    void InnerShadow(const RECT& r, int size, float alpha, COLORREF c = RGB(0, 0, 0))
    {
        if (!m_ok || size <= 0) return;
        RECT t = { r.left, r.top, r.right, r.top + size };
        RECT b = { r.left, r.bottom - size, r.right, r.bottom };
        RECT l = { r.left, r.top, r.left + size, r.bottom };
        RECT rr = { r.right - size, r.top, r.right, r.bottom };
        Gradient(t, c, alpha, c, 0.0f, true);
        GradientRev(b, c, alpha, true);
        Gradient(l, c, alpha, c, 0.0f, false);
        GradientRev(rr, c, alpha, false);
    }

    // The mirror of InnerShadow: light rising from the bottom and right, so
    // a pressed shape reads as pressed rather than merely darkened.
    void InnerLight(const RECT& r, int size, float alpha)
    {
        if (!m_ok || size <= 0) return;
        RECT b = { r.left, r.bottom - size, r.right, r.bottom };
        RECT rr = { r.right - size, r.top, r.right, r.bottom };
        GradientRev(b, RGB(255, 255, 255), alpha, true);
        GradientRev(rr, RGB(255, 255, 255), alpha, false);
    }

    // A glow: the colour at the centre, transparent at the radius.
    void Glow(float cx, float cy, float radius, COLORREF c, float alpha)
    {
        if (!m_ok || radius <= 0.0f) return;
        D2D1_GRADIENT_STOP st[2] = { { 0.0f, Col(c, alpha) }, { 1.0f, Col(c, 0.0f) } };
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(m_rt->CreateGradientStopCollection(st, 2, coll.GetAddressOf())))
            return;
        ComPtr<ID2D1RadialGradientBrush> b;
        m_rt->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx - m_ox, cy - m_oy),
                                                D2D1::Point2F(0, 0), radius, radius),
            coll.Get(), b.GetAddressOf());
        if (b)
            m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - m_ox, cy - m_oy), radius, radius),
                              b.Get());
    }

    // A radial wash over a rect: `centre` in the middle, `edge` at the corners.
    void Radial(const RECT& r, COLORREF centre, float a0, COLORREF edge, float a1)
    {
        if (!m_ok) return;
        D2D1_GRADIENT_STOP st[2] = { { 0.0f, Col(centre, a0) }, { 1.0f, Col(edge, a1) } };
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(m_rt->CreateGradientStopCollection(st, 2, coll.GetAddressOf())))
            return;
        const D2D1_RECT_F rf = R(r);
        const float cx = (rf.left + rf.right) * 0.5f, cy = (rf.top + rf.bottom) * 0.5f;
        const float rx = (rf.right - rf.left) * 0.72f, ry = (rf.bottom - rf.top) * 0.72f;
        ComPtr<ID2D1RadialGradientBrush> b;
        m_rt->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry),
            coll.Get(), b.GetAddressOf());
        if (b)
            m_rt->FillRectangle(rf, b.Get());
    }

    // A sunburst: hairline spokes from a centre, the way a brushed dial is
    // finished. Alternating spokes are lighter, so it reads as radial grain.
    void Sunburst(float cx, float cy, float radius, int spokes, COLORREF light, float alpha)
    {
        if (!m_ok || spokes <= 0) return;
        ComPtr<ID2D1SolidColorBrush> b;
        m_rt->CreateSolidColorBrush(Col(light, alpha), b.GetAddressOf());
        if (!b) return;
        for (int i = 0; i < spokes; ++i)
        {
            const float a = static_cast<float>(i) * 6.2831853f / spokes;
            m_rt->DrawLine(D2D1::Point2F(cx - m_ox, cy - m_oy),
                           D2D1::Point2F(cx - m_ox + std::cos(a) * radius,
                                         cy - m_oy + std::sin(a) * radius),
                           b.Get(), 0.8f);
        }
    }

    // Anti-aliased ellipse and line, for the places GDI's jaggies show.
    void Ellipse(float cx, float cy, float rx, float ry, COLORREF c, float alpha)
    {
        if (!m_ok) return;
        ComPtr<ID2D1SolidColorBrush> b;
        m_rt->CreateSolidColorBrush(Col(c, alpha), b.GetAddressOf());
        if (b)
            m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - m_ox, cy - m_oy), rx, ry), b.Get());
    }
    void Ring(float cx, float cy, float r, float width, COLORREF c, float alpha)
    {
        if (!m_ok) return;
        ComPtr<ID2D1SolidColorBrush> b;
        m_rt->CreateSolidColorBrush(Col(c, alpha), b.GetAddressOf());
        if (b)
            m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - m_ox, cy - m_oy), r, r), b.Get(), width);
    }
    void Line(float x0, float y0, float x1, float y1, float w, COLORREF c, float alpha)
    {
        if (!m_ok) return;
        ComPtr<ID2D1SolidColorBrush> b;
        m_rt->CreateSolidColorBrush(Col(c, alpha), b.GetAddressOf());
        if (b)
            m_rt->DrawLine(D2D1::Point2F(x0 - m_ox, y0 - m_oy), D2D1::Point2F(x1 - m_ox, y1 - m_oy),
                           b.Get(), w);
    }
    // A rounded rect fill with alpha — the anti-aliased plate under a control.
    void Rounded(const RECT& r, float radius, COLORREF c, float alpha)
    {
        if (!m_ok) return;
        ComPtr<ID2D1SolidColorBrush> b;
        m_rt->CreateSolidColorBrush(Col(c, alpha), b.GetAddressOf());
        if (b)
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(R(r), radius, radius), b.Get());
    }

private:
    D2D1_RECT_F R(const RECT& r) const
    {
        return D2D1::RectF(r.left - m_ox, r.top - m_oy, r.right - m_ox, r.bottom - m_oy);
    }
    // A gradient that is transparent at its start and `alpha` at its end.
    void GradientRev(const RECT& r, COLORREF c, float alpha, bool vertical)
    {
        Gradient(r, c, 0.0f, c, alpha, vertical);
    }

    ComPtr<ID2D1DCRenderTarget> m_rt;
    float m_ox = 0.0f, m_oy = 0.0f;
    bool m_ok = false;
};

} // namespace finish
} // namespace amber
