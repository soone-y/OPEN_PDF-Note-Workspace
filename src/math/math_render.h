#pragma once
#include <string>
#include <memory>
#include <vector>
#include <windows.h>

namespace mathrender {

// Controls how additional vertical margin (when needed to prevent super/sub collisions) is distributed.
// percent:
// - 0: auto (default bias)
// - 5..95: allocate this percent of the additional margin to superscripts, and the rest to subscripts
void SetSupSubGapSupPercent(int percent);
int GetSupSubGapSupPercent();

struct Node {
    enum class Type { Text, Fraction, Sqrt, Binom, SuperSub, Group, Matrix, Accent, Bracket };
    enum class ScriptLayoutPreference { Auto, Centered, RightScripts };
    Type type = Type::Text;
    ScriptLayoutPreference scriptLayout = ScriptLayoutPreference::Auto;
    std::wstring text;
    std::unique_ptr<Node> a;
    std::unique_ptr<Node> b;
    std::unique_ptr<Node> super;
    std::unique_ptr<Node> sub;
    std::vector<std::vector<std::unique_ptr<Node>>> rows;
    std::wstring leftDelimiter;
    std::wstring rightDelimiter;
    bool alignAtAmpersand = false;
    bool centerColumns = false;
};

struct Layout {
    int width = 0;
    int height = 0;
    int baseline = 0; // distance from top to baseline
};

enum class RenderStyle { Inline, Display };

std::unique_ptr<Node> Parse(std::wstring_view expr);
Layout Measure(const Node& n, HDC hdc, int fontPx, RenderStyle style = RenderStyle::Inline);
void Draw(const Node& n, const Layout& box, HDC hdc, int x, int y, int fontPx, COLORREF color,
          RenderStyle style = RenderStyle::Inline);

} // namespace mathrender
