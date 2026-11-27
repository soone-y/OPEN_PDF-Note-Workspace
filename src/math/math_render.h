#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace mathrender {
struct Layout {
    int width = 0;
    int height = 0;
    int baseline = 0;
};
enum class RenderStyle { Inline, Display };

// Opaque parsed source. Its implementation owns the engine lifetime and prepared
// variants. A view or completed frame may retain it independently of cache eviction.
class Formula final {
public:
    ~Formula();
    Formula(const Formula&) = delete;
    Formula& operator=(const Formula&) = delete;
private:
    struct Data;
    std::unique_ptr<Data> data_;
    explicit Formula(std::unique_ptr<Data> data) noexcept;
    friend std::unique_ptr<Formula> Parse(std::wstring_view, std::string*) noexcept;
    friend Layout Measure(const Formula&, HDC, int, RenderStyle) noexcept;
    friend bool Draw(const Formula&, const Layout&, HDC, int, int, int, COLORREF, RenderStyle) noexcept;
};

[[nodiscard]] std::unique_ptr<Formula> Parse(std::wstring_view expression,
                                          std::string* diagnostic = nullptr) noexcept;
[[nodiscard]] Layout Measure(const Formula& formula, HDC dc, int fontPx,
                             RenderStyle style = RenderStyle::Inline) noexcept;
[[nodiscard]] bool Draw(const Formula& formula, const Layout& layout, HDC dc,
                        int x, int y, int fontPx, COLORREF colour,
                        RenderStyle style = RenderStyle::Inline) noexcept;
// Fit uniformly into an annotation box, composing with the borrowed DC's
// existing zoom/rotation. The caller retains its source and handles failure.
[[nodiscard]] bool DrawInBox(const Formula& formula, HDC dc, const RECT& bounds,
                             int fontPx, COLORREF colour,
                             RenderStyle style = RenderStyle::Display) noexcept;
} // namespace mathrender
