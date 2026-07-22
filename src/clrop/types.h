#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clrop {

struct PdfId {
    std::wstring path;
    std::uint64_t size = 0;
    int pageCount = 0;
    std::vector<std::array<double, 2>> pageSizesPt;
    std::string sha256; // hex lowercase
};

struct Item {
    enum class Kind { Text, Math, MarkerText, TextColor, MarkerFree, Line, Arrow, Wave, Freehand, LinkMarker, Shape } kind = Kind::MarkerText;
    std::wstring id;
    std::wstring created; // ISO8601
    std::wstring updated; // ISO8601
    std::string color;    // "#RRGGBB"
    double alpha = 1.0;
    double width = 0.0;
    double pt = 0.0;      // font size for text
    std::wstring font;
    std::string border;   // "none" or "rect"
    std::wstring content; // text content
    std::vector<std::wstring> lines;
    std::string writingMode; // "horizontal" or "vertical_rl"; empty means horizontal
    std::wstring linkId;
    std::wstring notePath;
    std::string mathKind;
    std::string shapeKind;
    std::string shapeDrawMode;
    std::string arrowHead; // "single" or "double"; empty means single for legacy files
    std::optional<double> shapeRotation; // radians; used by rotated_ellipse

    std::optional<std::array<double, 4>> bbox;   // x,y,w,h (text)
    std::optional<std::array<double, 2>> p1;     // line start
    std::optional<std::array<double, 2>> p2;     // line end
    std::vector<double> quads;                   // marker-text / text-color, 8*N
    std::vector<double> path;                    // marker-free/freehand, [x0,y0,x1,y1,...]
    std::vector<double> dash;                    // line dash
};

struct Page {
    int page = 0;
    std::vector<Item> items;
};

struct Document {
    int version = 1;
    PdfId pdfId;
    std::vector<Page> pages;
    bool dirty = false;
};

} // namespace clrop
