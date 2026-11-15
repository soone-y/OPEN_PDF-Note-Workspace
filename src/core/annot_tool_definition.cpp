#include "core/annot_tool_definition.h"

#include <algorithm>
#include <cctype>

std::vector<ToolMode> DefaultAnnotToolModeUiOrder() {
    return {
        ToolMode::Select,
        ToolMode::Pan,
        ToolMode::Magnifier,
        ToolMode::TextBox,
        ToolMode::MarkerText,
        ToolMode::MarkerTextUnderline,
        ToolMode::MarkerTextColor,
        ToolMode::MarkerFree,
        ToolMode::Freehand,
        ToolMode::Line,
        ToolMode::Wave,
        ToolMode::Arrow,
        ToolMode::Shape,
        ToolMode::MarkerLine,
        ToolMode::MarkerWave,
        ToolMode::MarkerArrow,
        ToolMode::Eraser,
    };
}

static const std::vector<AnnotToolFamily>& DefaultAnnotToolFamilyUiOrder() {
    static const std::vector<AnnotToolFamily> order = {
        AnnotToolFamily::Select,
        AnnotToolFamily::Pan,
        AnnotToolFamily::Magnifier,
        AnnotToolFamily::Text,
        AnnotToolFamily::Marker,
        AnnotToolFamily::Pen,
        AnnotToolFamily::Shape,
        AnnotToolFamily::Eraser,
    };
    return order;
}

const char* AnnotToolModeKey(ToolMode mode) {
    switch (mode) {
    case ToolMode::Select:     return "select";
    case ToolMode::Pan:        return "pan";
    case ToolMode::Magnifier:  return "magnifier";
    case ToolMode::MarkerText: return "marker_text";
    case ToolMode::MarkerTextUnderline: return "marker_text_underline";
    case ToolMode::MarkerTextColor: return "marker_text_color";
    case ToolMode::MarkerFree: return "marker_free";
    case ToolMode::MarkerLine: return "marker_line";
    case ToolMode::MarkerArrow:return "marker_arrow";
    case ToolMode::MarkerWave: return "marker_wave";
    case ToolMode::TextBox:    return "text";
    case ToolMode::Line:       return "line";
    case ToolMode::Arrow:      return "arrow";
    case ToolMode::Wave:       return "wave";
    case ToolMode::Freehand:   return "freehand";
    case ToolMode::Shape:      return "shape";
    case ToolMode::Eraser:     return "eraser";
    default:                   return "";
    }
}

bool AnnotToolModeFromKey(const std::string& raw, ToolMode& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "select") { out = ToolMode::Select; return true; }
    if (key == "pan") { out = ToolMode::Pan; return true; }
    if (key == "magnifier") { out = ToolMode::Magnifier; return true; }
    if (key == "marker_text" || key == "markertext") { out = ToolMode::MarkerText; return true; }
    if (key == "marker_text_underline" || key == "markertextunderline" ||
        key == "marker_underline" || key == "markerunderline") { out = ToolMode::MarkerTextUnderline; return true; }
    if (key == "marker_text_color" || key == "markertextcolor" ||
        key == "marker_color_text" || key == "markercolortext" ||
        key == "text_color_marker" || key == "textcolormarker") { out = ToolMode::MarkerTextColor; return true; }
    if (key == "marker_free" || key == "markerfree") { out = ToolMode::MarkerFree; return true; }
    if (key == "marker_line" || key == "markerline") { out = ToolMode::MarkerLine; return true; }
    if (key == "marker_arrow" || key == "markerarrow") { out = ToolMode::MarkerArrow; return true; }
    if (key == "marker_wave" || key == "markerwave") { out = ToolMode::MarkerWave; return true; }
    if (key == "text" || key == "textbox") { out = ToolMode::TextBox; return true; }
    if (key == "line") { out = ToolMode::Line; return true; }
    if (key == "arrow") { out = ToolMode::Arrow; return true; }
    if (key == "wave") { out = ToolMode::Wave; return true; }
    if (key == "freehand") { out = ToolMode::Freehand; return true; }
    if (key == "shape") { out = ToolMode::Shape; return true; }
    if (key == "eraser") { out = ToolMode::Eraser; return true; }
    return false;
}

const char* AnnotToolFamilyKey(AnnotToolFamily family) {
    switch (family) {
    case AnnotToolFamily::Select: return "select";
    case AnnotToolFamily::Pan: return "pan";
    case AnnotToolFamily::Magnifier: return "magnifier";
    case AnnotToolFamily::Text: return "text";
    case AnnotToolFamily::Marker: return "marker";
    case AnnotToolFamily::Pen: return "pen";
    case AnnotToolFamily::Shape: return "shape";
    case AnnotToolFamily::Eraser: return "eraser";
    default: return "";
    }
}

bool AnnotToolFamilyFromKey(const std::string& raw, AnnotToolFamily& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "select") { out = AnnotToolFamily::Select; return true; }
    if (key == "pan") { out = AnnotToolFamily::Pan; return true; }
    if (key == "magnifier") { out = AnnotToolFamily::Magnifier; return true; }
    if (key == "text" || key == "textbox") { out = AnnotToolFamily::Text; return true; }
    if (key == "marker") { out = AnnotToolFamily::Marker; return true; }
    if (key == "pen" || key == "freehand") { out = AnnotToolFamily::Pen; return true; }
    if (key == "shape") { out = AnnotToolFamily::Shape; return true; }
    if (key == "eraser") { out = AnnotToolFamily::Eraser; return true; }
    return false;
}

const std::vector<AnnotToolFamily>& AnnotToolFamilyUiOrder() {
    return DefaultAnnotToolFamilyUiOrder();
}

AnnotToolFamily AnnotToolFamilyForMode(ToolMode mode) {
    switch (mode) {
    case ToolMode::Select:
        return AnnotToolFamily::Select;
    case ToolMode::Pan:
        return AnnotToolFamily::Pan;
    case ToolMode::Magnifier:
        return AnnotToolFamily::Magnifier;
    case ToolMode::TextBox:
        return AnnotToolFamily::Text;
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::MarkerFree:
        return AnnotToolFamily::Marker;
    case ToolMode::Freehand:
        return AnnotToolFamily::Pen;
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::Line:
    case ToolMode::MarkerWave:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Shape:
        return AnnotToolFamily::Shape;
    case ToolMode::Eraser:
    default:
        return AnnotToolFamily::Eraser;
    }
}

const std::vector<AnnotToolDetailDescriptor>& AnnotToolDetailDescriptors() {
    static const std::vector<AnnotToolDetailDescriptor> descriptors = [] {
        std::vector<AnnotToolDetailDescriptor> out;
        out.reserve(kToolModeCount);
        for (ToolMode mode : DefaultAnnotToolModeUiOrder()) {
            AnnotToolOptionGroup optionGroup = AnnotToolOptionGroup::None;
            AnnotToolGeometry geometry = AnnotToolGeometry::None;
            AnnotToolPresentation presentation = AnnotToolPresentation::None;
            switch (mode) {
            case ToolMode::Magnifier: optionGroup = AnnotToolOptionGroup::Magnifier; break;
            case ToolMode::TextBox: optionGroup = AnnotToolOptionGroup::Text; break;
            case ToolMode::MarkerText:
            case ToolMode::MarkerTextUnderline:
            case ToolMode::MarkerTextColor: optionGroup = AnnotToolOptionGroup::MarkerText; break;
            case ToolMode::MarkerFree: optionGroup = AnnotToolOptionGroup::MarkerFree; break;
            case ToolMode::MarkerLine:
                optionGroup = AnnotToolOptionGroup::Line;
                geometry = AnnotToolGeometry::Line;
                presentation = AnnotToolPresentation::Emphasis;
                break;
            case ToolMode::MarkerArrow:
                optionGroup = AnnotToolOptionGroup::Arrow;
                geometry = AnnotToolGeometry::Arrow;
                presentation = AnnotToolPresentation::Emphasis;
                break;
            case ToolMode::MarkerWave:
                optionGroup = AnnotToolOptionGroup::Wave;
                geometry = AnnotToolGeometry::Wave;
                presentation = AnnotToolPresentation::Emphasis;
                break;
            case ToolMode::Line:
                optionGroup = AnnotToolOptionGroup::Line;
                geometry = AnnotToolGeometry::Line;
                presentation = AnnotToolPresentation::Stroke;
                break;
            case ToolMode::Arrow:
                optionGroup = AnnotToolOptionGroup::Arrow;
                geometry = AnnotToolGeometry::Arrow;
                presentation = AnnotToolPresentation::Stroke;
                break;
            case ToolMode::Wave:
                optionGroup = AnnotToolOptionGroup::Wave;
                geometry = AnnotToolGeometry::Wave;
                presentation = AnnotToolPresentation::Stroke;
                break;
            case ToolMode::Freehand: optionGroup = AnnotToolOptionGroup::Pen; break;
            case ToolMode::Shape:
                optionGroup = AnnotToolOptionGroup::Shape;
                geometry = AnnotToolGeometry::Shape;
                break;
            default: break;
            }
            out.push_back({mode, AnnotToolFamilyForMode(mode), AnnotToolModeKey(mode), optionGroup, geometry, presentation});
        }
        return out;
    }();
    return descriptors;
}

const AnnotToolDetailDescriptor* FindAnnotToolDetailDescriptor(ToolMode mode) {
    const auto& descriptors = AnnotToolDetailDescriptors();
    auto it = std::find_if(descriptors.begin(), descriptors.end(), [mode](const AnnotToolDetailDescriptor& descriptor) {
        return descriptor.mode == mode;
    });
    return it == descriptors.end() ? nullptr : &*it;
}

AnnotToolGeometry AnnotToolGeometryForMode(ToolMode mode) {
    const auto* descriptor = FindAnnotToolDetailDescriptor(mode);
    return descriptor ? descriptor->geometry : AnnotToolGeometry::None;
}

AnnotToolPresentation AnnotToolPresentationForMode(ToolMode mode, ShapeDrawMode shapeDrawMode) {
    if (mode == ToolMode::Shape) {
        return shapeDrawMode == ShapeDrawMode::Fill
            ? AnnotToolPresentation::Emphasis
            : AnnotToolPresentation::Stroke;
    }
    const auto* descriptor = FindAnnotToolDetailDescriptor(mode);
    return descriptor ? descriptor->presentation : AnnotToolPresentation::None;
}

ShapeToolSelection ShapeToolSelectionForMode(ToolMode mode, ShapeDrawMode shapeDrawMode) {
    return { AnnotToolPresentationForMode(mode, shapeDrawMode), AnnotToolGeometryForMode(mode) };
}

std::optional<ToolMode> ToolModeForShapeToolSelection(ShapeToolSelection selection) {
    if (selection.presentation != AnnotToolPresentation::Stroke &&
        selection.presentation != AnnotToolPresentation::Emphasis) {
        return std::nullopt;
    }
    switch (selection.geometry) {
    case AnnotToolGeometry::Line:
        return selection.presentation == AnnotToolPresentation::Emphasis ? ToolMode::MarkerLine : ToolMode::Line;
    case AnnotToolGeometry::Wave:
        return selection.presentation == AnnotToolPresentation::Emphasis ? ToolMode::MarkerWave : ToolMode::Wave;
    case AnnotToolGeometry::Arrow:
        return selection.presentation == AnnotToolPresentation::Emphasis ? ToolMode::MarkerArrow : ToolMode::Arrow;
    case AnnotToolGeometry::Shape:
        return ToolMode::Shape;
    default:
        return std::nullopt;
    }
}

const char* ShapeToolPresentationKey(AnnotToolPresentation presentation) {
    switch (presentation) {
    case AnnotToolPresentation::Stroke: return "stroke";
    case AnnotToolPresentation::Emphasis: return "emphasis";
    default: return "stroke";
    }
}

bool ShapeToolPresentationFromKey(const std::string& raw, AnnotToolPresentation& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "stroke") { out = AnnotToolPresentation::Stroke; return true; }
    if (key == "emphasis") { out = AnnotToolPresentation::Emphasis; return true; }
    return false;
}

const char* ShapeToolGeometryKey(AnnotToolGeometry geometry) {
    switch (geometry) {
    case AnnotToolGeometry::Line: return "line";
    case AnnotToolGeometry::Wave: return "wave";
    case AnnotToolGeometry::Arrow: return "arrow";
    case AnnotToolGeometry::Shape: return "shape";
    default: return "line";
    }
}

bool ShapeToolGeometryFromKey(const std::string& raw, AnnotToolGeometry& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "line") { out = AnnotToolGeometry::Line; return true; }
    if (key == "wave") { out = AnnotToolGeometry::Wave; return true; }
    if (key == "arrow") { out = AnnotToolGeometry::Arrow; return true; }
    if (key == "shape") { out = AnnotToolGeometry::Shape; return true; }
    return false;
}

const char* ShapeDetailKey(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Line: return "line";
    case ShapeDetail::Arrow: return "arrow";
    case ShapeDetail::Wave: return "wave";
    case ShapeDetail::Rectangle: return "rectangle";
    case ShapeDetail::Ellipse: return "ellipse";
    case ShapeDetail::Triangle: return "triangle";
    case ShapeDetail::Diamond: return "diamond";
    case ShapeDetail::Arc: return "arc";
    case ShapeDetail::Star: return "star";
    default: return "line";
    }
}

bool ShapeDetailFromKey(const std::string& raw, ShapeDetail& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "line" || key == "marker_line") { out = ShapeDetail::Line; return true; }
    if (key == "arrow" || key == "marker_arrow") { out = ShapeDetail::Arrow; return true; }
    if (key == "wave" || key == "marker_wave") { out = ShapeDetail::Wave; return true; }
    if (key == "shape" || key == "rect" || key == "rectangle" || key == "square") {
        out = ShapeDetail::Rectangle;
        return true;
    }
    if (key == "ellipse" || key == "circle" || key == "rotated_ellipse") {
        out = ShapeDetail::Ellipse;
        return true;
    }
    if (key == "triangle" || key == "equilateral_triangle") {
        out = ShapeDetail::Triangle;
        return true;
    }
    if (key == "diamond") { out = ShapeDetail::Diamond; return true; }
    if (key == "arc") { out = ShapeDetail::Arc; return true; }
    if (key == "star") { out = ShapeDetail::Star; return true; }
    return false;
}

std::vector<ShapeDetail> OrderedShapeDetails() {
    return {
        ShapeDetail::Line,
        ShapeDetail::Arrow,
        ShapeDetail::Wave,
        ShapeDetail::Rectangle,
        ShapeDetail::Ellipse,
        ShapeDetail::Triangle,
        ShapeDetail::Diamond,
        ShapeDetail::Arc,
        ShapeDetail::Star,
    };
}

bool ShapeDetailIsLinear(ShapeDetail detail) {
    return detail == ShapeDetail::Line ||
           detail == ShapeDetail::Arrow ||
           detail == ShapeDetail::Wave;
}

bool ShapeDetailUsesBox(ShapeDetail detail) {
    return !ShapeDetailIsLinear(detail);
}

ToolMode ToolModeForShapeDetail(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Line: return ToolMode::Line;
    case ShapeDetail::Arrow: return ToolMode::Arrow;
    case ShapeDetail::Wave: return ToolMode::Wave;
    case ShapeDetail::Rectangle:
    case ShapeDetail::Ellipse:
    case ShapeDetail::Triangle:
    case ShapeDetail::Diamond:
    case ShapeDetail::Arc:
    case ShapeDetail::Star:
        return ToolMode::Shape;
    default:
        return ToolMode::Line;
    }
}

ShapeKind ShapeKindForShapeDetail(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Ellipse: return ShapeKind::Ellipse;
    case ShapeDetail::Triangle: return ShapeKind::Triangle;
    case ShapeDetail::Diamond: return ShapeKind::Diamond;
    case ShapeDetail::Arc: return ShapeKind::Arc;
    case ShapeDetail::Star: return ShapeKind::Star;
    case ShapeDetail::Rectangle:
    default:
        return ShapeKind::Rectangle;
    }
}

ShapeDetail ShapeDetailForLegacyState(ToolMode mode, ShapeKind kind) {
    switch (mode) {
    case ToolMode::MarkerLine:
    case ToolMode::Line:
        return ShapeDetail::Line;
    case ToolMode::MarkerArrow:
    case ToolMode::Arrow:
        return ShapeDetail::Arrow;
    case ToolMode::MarkerWave:
    case ToolMode::Wave:
        return ShapeDetail::Wave;
    case ToolMode::Shape:
        switch (kind) {
        case ShapeKind::Ellipse:
        case ShapeKind::Circle:
        case ShapeKind::RotatedEllipse:
            return ShapeDetail::Ellipse;
        case ShapeKind::Triangle:
        case ShapeKind::EquilateralTriangle:
            return ShapeDetail::Triangle;
        case ShapeKind::Diamond:
            return ShapeDetail::Diamond;
        case ShapeKind::Arc:
            return ShapeDetail::Arc;
        case ShapeKind::Star:
            return ShapeDetail::Star;
        case ShapeKind::Rectangle:
        case ShapeKind::Square:
        default:
            return ShapeDetail::Rectangle;
        }
    default:
        return ShapeDetail::Line;
    }
}

bool AnnotToolUsesGeometry(ToolMode mode, AnnotToolGeometry geometry) {
    return geometry != AnnotToolGeometry::None && AnnotToolGeometryForMode(mode) == geometry;
}

bool IsLineLikeAnnotToolMode(ToolMode mode) {
    const AnnotToolGeometry geometry = AnnotToolGeometryForMode(mode);
    return geometry == AnnotToolGeometry::Line ||
           geometry == AnnotToolGeometry::Wave ||
           geometry == AnnotToolGeometry::Arrow;
}

bool IsArrowAnnotToolMode(ToolMode mode) {
    return AnnotToolUsesGeometry(mode, AnnotToolGeometry::Arrow);
}

bool IsWaveAnnotToolMode(ToolMode mode) {
    return AnnotToolUsesGeometry(mode, AnnotToolGeometry::Wave);
}

bool IsEmphasisAnnotToolMode(ToolMode mode, ShapeDrawMode shapeDrawMode) {
    return AnnotToolPresentationForMode(mode, shapeDrawMode) == AnnotToolPresentation::Emphasis;
}
