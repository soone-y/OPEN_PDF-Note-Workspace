// The upstream openmath GDI backend leaves glyph path callbacks empty.
// Complete those callbacks locally for the offscreen comparison.
#pragma once

class OpenmathPathGraphics final : public microtex::Graphics2D_win32 {
    Gdiplus::Graphics* graphics_;
    Gdiplus::GraphicsPath path_{Gdiplus::FillModeWinding};
    Gdiplus::PointF point_{};
    Gdiplus::PointF contourStart_{};

public:
    explicit OpenmathPathGraphics(Gdiplus::Graphics* graphics)
        : microtex::Graphics2D_win32(graphics), graphics_(graphics) {}

    bool beginPath(microtex::i32) override {
        path_.Reset();
        path_.SetFillMode(Gdiplus::FillModeWinding);
        return false;
    }

    void moveTo(float x, float y) override {
        path_.StartFigure();
        point_ = contourStart_ = Gdiplus::PointF(x, y);
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
        point_ = contourStart_;
    }

    void fillPath(microtex::i32) override {
        Gdiplus::SolidBrush brush{Gdiplus::Color(getColor())};
        if (graphics_->FillPath(&brush, &path_) != Gdiplus::Ok) {
            throw std::runtime_error("GDI+ glyph path fill failed");
        }
    }
};
