#include "math/math_render_internal.h"

#include <gdiplus.h>
#include "graphic_win32.h"

#include <stdexcept>

namespace mathrender::detail {
namespace {

// Upstream's Windows backend leaves the outline callbacks unimplemented.
// This adapter owns a path for one glyph and borrows the drawing surface.
class PathGraphics final : public microtex::Graphics2D_win32 {
    Gdiplus::Graphics& graphics_;
    Gdiplus::GraphicsPath path_;
    Gdiplus::PointF point_{};
    Gdiplus::PointF start_{};
public:
    explicit PathGraphics(Gdiplus::Graphics& graphics)
        : microtex::Graphics2D_win32(&graphics), graphics_(graphics) {}

    bool beginPath(microtex::i32) override {
        path_.Reset();
        path_.SetFillMode(Gdiplus::FillModeWinding);
        return false;
    }
    void moveTo(float x, float y) override {
        path_.StartFigure();
        point_ = start_ = Gdiplus::PointF(x, y);
    }
    void lineTo(float x, float y) override {
        const Gdiplus::PointF end(x, y);
        path_.AddLine(point_, end);
        point_ = end;
    }
    void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) override {
        const Gdiplus::PointF end(x3, y3);
        path_.AddBezier(point_, Gdiplus::PointF(x1, y1), Gdiplus::PointF(x2, y2), end);
        point_ = end;
    }
    void quadTo(float x1, float y1, float x2, float y2) override {
        cubicTo(point_.X + (x1 - point_.X) * 2.f / 3.f,
                point_.Y + (y1 - point_.Y) * 2.f / 3.f,
                x2 + (x1 - x2) * 2.f / 3.f,
                y2 + (y1 - y2) * 2.f / 3.f, x2, y2);
    }
    void closePath() override {
        path_.CloseFigure();
        point_ = start_;
    }
    void fillPath(microtex::i32) override {
        Gdiplus::SolidBrush brush{Gdiplus::Color(getColor())};
        if (graphics_.FillPath(&brush, &path_) != Gdiplus::Ok) {
            throw std::runtime_error("Math outline drawing failed");
        }
    }
};
} // namespace

GdiSession::GdiSession() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        throw std::runtime_error("Math graphics initialization failed");
    }
}
GdiSession::~GdiSession() { Gdiplus::GdiplusShutdown(token); }

void RegisterWindowsBackend() {
    microtex::PlatformFactory::registerFactory("windows", std::make_unique<microtex::PlatformFactory_gdi>());
    microtex::PlatformFactory::activate("windows");
}

void ReleaseWindowsBackend() noexcept {
    microtex::Font_win32::clearCache();
    delete microtex::TextLayout_win32::_g;
    microtex::TextLayout_win32::_g = nullptr;
    delete microtex::TextLayout_win32::_img;
    microtex::TextLayout_win32::_img = nullptr;
}

bool DrawPrepared(microtex::Render& render, HDC dc, int x, int y, COLORREF colour) noexcept {
    if (!dc) return false;
    try {
        // One context per draw prevents round-off accumulating across repaint calls.
        Gdiplus::Graphics graphics(dc);
        if (graphics.GetLastStatus() != Gdiplus::Ok) return false;
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        const auto state = graphics.Save();
        PathGraphics context(graphics);
        render.setForeground(0xff000000u | (GetRValue(colour) << 16) |
                             (GetGValue(colour) << 8) | GetBValue(colour));
        render.draw(context, x, y);
        graphics.Restore(state);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        return graphics.GetLastStatus() == Gdiplus::Ok;
    } catch (...) {
        return false;
    }
}
} // namespace mathrender::detail
