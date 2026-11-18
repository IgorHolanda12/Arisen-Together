#pragma once
#include <windows.h>
#include <gdiplus.h>

namespace theme {

constexpr COLORREF kBg = RGB(0x0f, 0x0d, 0x0b);
constexpr COLORREF kTopbar = RGB(0x16, 0x13, 0x10);
constexpr COLORREF kSurface = RGB(0x1b, 0x17, 0x14);
constexpr COLORREF kSurfaceHover = RGB(0x24, 0x1f, 0x1a);
constexpr COLORREF kSurfaceRaised = RGB(0x2c, 0x26, 0x20);
constexpr COLORREF kBorder = RGB(0x34, 0x2c, 0x25);
constexpr COLORREF kAccent = RGB(0xc9, 0xa2, 0x4a);
constexpr COLORREF kAccentHover = RGB(0xdc, 0xb6, 0x5e);
constexpr COLORREF kAccentDeep = RGB(0xa8, 0x82, 0x36);
constexpr COLORREF kOnAccent = RGB(0x17, 0x11, 0x0a);
constexpr COLORREF kText = RGB(0xe9, 0xe1, 0xd0);
constexpr COLORREF kMuted = RGB(0xa5, 0x9c, 0x8b);
constexpr COLORREF kOk = RGB(0x84, 0xc4, 0x8c);
constexpr COLORREF kWarn = RGB(0xe6, 0x96, 0x4a);
constexpr COLORREF kBad = RGB(0xe2, 0x6a, 0x5c);
constexpr COLORREF kInfo = RGB(0x88, 0xae, 0xdb);
constexpr COLORREF kFocus = RGB(0xf2, 0xe7, 0xcf);

constexpr int kRadiusCard = 10, kRadiusButton = 7, kRadiusSmall = 5;
constexpr int kPadCard = 18, kGapSection = 24, kGapSmall = 12;

constexpr const wchar_t *kSerifFonts[] = {L"Palatino Linotype", L"Book Antiqua", L"Georgia", L"Cambria"};
constexpr const wchar_t *kFontFamily = L"Segoe UI Variable Text";
constexpr const wchar_t *kFontFallback = L"Segoe UI";
constexpr int kSizeSmall = 9, kSizeBody = 10, kSizeTab = 11, kSizeTitle = 13, kSizeWordmark = 14;

constexpr int kHoverMs = 160;

inline Gdiplus::Color gp(COLORREF c, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

inline COLORREF mix(COLORREF a, COLORREF b, float t)
{
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    return RGB(int(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), int(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               int(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

inline void round_path(Gdiplus::GraphicsPath &p, const RECT &r, int radius)
{
    float x = float(r.left), y = float(r.top), w = float(r.right - r.left - 1), h = float(r.bottom - r.top - 1);
    float d = float(radius * 2);
    if (d > w)
        d = w;
    if (d > h)
        d = h;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

inline void round_rect(HDC dc, const RECT &r, int radius, COLORREF fill, COLORREF border = CLR_INVALID, float width = 1.f,
                       bool dashed = false)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    round_path(p, r, radius);
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(gp(fill));
        g.FillPath(&b, &p);
    }
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(gp(border), width);
        if (dashed)
            pen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawPath(&pen, &p);
    }
}

inline void round_rect_v(HDC dc, const RECT &r, int radius, COLORREF top, COLORREF bottom, COLORREF border = CLR_INVALID)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    round_path(p, r, radius);
    Gdiplus::LinearGradientBrush b(Gdiplus::Point(0, INT(r.top)), Gdiplus::Point(0, INT(r.bottom)), gp(top), gp(bottom));
    g.FillPath(&b, &p);
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(gp(border), 1.f);
        g.DrawPath(&pen, &p);
    }
}

inline void circle(HDC dc, const RECT &r, COLORREF fill, COLORREF ring = CLR_INVALID, float width = 1.f, bool dashed = false)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float x = float(r.left), y = float(r.top), w = float(r.right - r.left - 1), h = float(r.bottom - r.top - 1);
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(gp(fill));
        g.FillEllipse(&b, x, y, w, h);
    }
    if (ring != CLR_INVALID) {
        Gdiplus::Pen pen(gp(ring), width);
        if (dashed)
            pen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawEllipse(&pen, x + width / 2, y + width / 2, w - width, h - width);
    }
}

inline void circle_picture(HDC dc, const RECT &r, HBITMAP pic)
{
    Gdiplus::Bitmap *src = Gdiplus::Bitmap::FromHBITMAP(pic, nullptr);
    if (!src)
        return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    float w = float(r.right - r.left), h = float(r.bottom - r.top);
    Gdiplus::TextureBrush b(src);
    b.TranslateTransform(float(r.left), float(r.top));
    b.ScaleTransform(w / float(src->GetWidth()), h / float(src->GetHeight()));
    g.FillEllipse(&b, float(r.left), float(r.top), w - 1, h - 1);
    delete src;
}

inline void wakestone(HDC dc, const RECT &r)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float x = float(r.left), y = float(r.top), w = float(r.right - r.left), h = float(r.bottom - r.top);
    Gdiplus::PointF top(x + w * 0.50f, y + h * 0.03f), right(x + w * 0.80f, y + h * 0.40f), bottom(x + w * 0.50f, y + h * 0.97f),
        left(x + w * 0.20f, y + h * 0.40f), mid(x + w * 0.52f, y + h * 0.46f);
    Gdiplus::PointF lf[4] = {top, left, bottom, mid}, rf[4] = {top, mid, bottom, right};
    Gdiplus::SolidBrush light(gp(RGB(0xe8, 0xc8, 0x78))), dark(gp(kAccentDeep));
    g.FillPolygon(&light, lf, 4);
    g.FillPolygon(&dark, rf, 4);
    Gdiplus::PointF glint[3] = {Gdiplus::PointF(x + w * 0.50f, y + h * 0.12f), Gdiplus::PointF(x + w * 0.31f, y + h * 0.40f),
                                Gdiplus::PointF(x + w * 0.47f, y + h * 0.42f)};
    Gdiplus::SolidBrush shine(gp(RGB(0xfb, 0xec, 0xc4), 200));
    g.FillPolygon(&shine, glint, 3);
}

}
