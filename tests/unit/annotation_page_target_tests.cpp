#include "core/annotation_page_target.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

int checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
bool Near(double a, double b) { return std::abs(a-b) < 1e-9; }
int main() {
    using namespace annotation_page_target;
    try {
        Check(Parse(L"1", 17, 2) == 0 && Parse(L" 2\t", 17, 2) == 1, "one-based page input maps to zero-based model pages");
        Check(Parse(L"18", 17, 2) == 17, "unchanged hidden source page is retained");
        for (const auto* text : {L"", L" ", L"0", L"-1", L"+1", L"1.5", L"1e0", L"3", L"2147483649", L"999999999999999999"})
            Check(!Parse(text, 17, 2), "invalid or new nonexistent page is rejected");
        Check(Parse(L"2147483648", std::numeric_limits<int>::max(), 2) == std::numeric_limits<int>::max(), "largest retained legacy index does not overflow");
        Check(!Parse(L"1", 17, 0), "missing PDF page range is rejected");
        for (auto type : {Annotation::Type::TextBox, Annotation::Type::MathBox, Annotation::Type::MarkerText,
            Annotation::Type::TextColor, Annotation::Type::MarkerFree, Annotation::Type::Freehand,
            Annotation::Type::Line, Annotation::Type::Arrow, Annotation::Type::Wave,
            Annotation::Type::LinkMarker, Annotation::Type::Shape}) {
            Annotation original; original.type = type; original.pageIndex = 17; original.id = L"stable-id";
            original.text = L"keep source"; original.linkId = L"keep-link"; original.width = 3; original.alpha = 0.7;
            original.x1 = 30; original.y1 = 120; original.x2 = 180; original.y2 = 80;
            original.path = {{40, 50}, {70, 110}}; original.quads = {30, 120, 180, 120, 180, 80, 30, 80};
            original.contentRotation = 2;
            for (int rotation = 0; rotation < 4; ++rotation) {
                const auto projected = annotation_rotation::InBasis(original, rotation, 600, 800, 20, -10);
                Check(projected.has_value(), "prepare a real projected coordinate basis");
                const auto moved = Retarget(*projected, 0, 2);
                Check(moved && moved->pageIndex == 0 && !moved->projectedCoordinates, "move canonicalizes the old page basis");
                Check(moved->id == original.id && moved->text == original.text && moved->linkId == original.linkId &&
                    moved->width == original.width && moved->alpha == original.alpha, "page move preserves identity, content and style");
                Check(Near(moved->x1,original.x1) && Near(moved->x2,original.x2) &&
                    Near(moved->y1,original.y1) && Near(moved->y2,original.y2), "page move preserves original PDF-space bounds");
                Check(moved->path.size() == original.path.size() && Near(moved->path[0].x, original.path[0].x) &&
                    Near(moved->path[0].y,original.path[0].y), "path coordinates survive page retargeting");
                for (size_t i=0;i<original.quads.size();++i) Check(Near(moved->quads[i],original.quads[i]), "text-marker quad order and coordinates are retained");
                Check(moved->contentRotation == original.contentRotation, "content orientation is retained");
                Check(!Retarget(*projected, 2, 2), "new out-of-range destination is refused");
                Check(projected->pageIndex == 17, "retargeting never mutates the source value");
            }
        }
        Annotation invalid; invalid.pageIndex = 17; invalid.projectedCoordinates = true; invalid.basisWidthPt = 0;
        Check(!Retarget(invalid, 0, 2), "invalid coordinate basis fails without relocating data");
        std::cout << "PASS: annotation page targeting / " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
