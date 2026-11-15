#pragma once

#include <optional>
#include <string>
#include <vector>

// Value definitions shared by annotation data and tool selection.
enum class ShapeKind {
    Square,
    Rectangle,
    Diamond,
    EquilateralTriangle,
    Triangle,
    Ellipse,
    Circle,
    RotatedEllipse,
    Arc, // open parenthesis-like curve, v2
    Star
};

enum class ShapeDrawMode {
    Fill,
    Outline,
    Line // five straight segments of a pentagram (Star only)
};

enum class ToolMode {
    Select,
    Pan,
    Magnifier,
    MarkerText,
    MarkerTextUnderline,
    MarkerTextColor,
    MarkerFree,
    MarkerLine,
    MarkerArrow,
    MarkerWave,
    TextBox,
    Line,
    Arrow,
    Wave,
    Freehand,
    Shape,
    Eraser
};
inline constexpr int kToolModeCount = static_cast<int>(ToolMode::Eraser) + 1;

enum class AnnotToolFamily {
    Select,
    Pan,
    Magnifier,
    Text,
    Marker,
    Pen,
    Shape,
    Eraser
};
inline constexpr int kAnnotToolFamilyCount = static_cast<int>(AnnotToolFamily::Eraser) + 1;

enum class AnnotToolGeometry { None, Shape, Line, Wave, Arrow };
enum class AnnotToolPresentation { None, Stroke, Emphasis };
enum class ShapeDetail { Line, Arrow, Wave, Rectangle, Ellipse, Triangle, Diamond, Arc, Star };
struct ShapeToolSelection {
    AnnotToolPresentation presentation = AnnotToolPresentation::Stroke;
    AnnotToolGeometry geometry = AnnotToolGeometry::Line;

    constexpr bool operator==(const ShapeToolSelection& other) const {
        return presentation == other.presentation && geometry == other.geometry;
    }
};
enum class AnnotToolOptionGroup {
    None,
    Magnifier,
    Text,
    MarkerText,
    MarkerFree,
    Line,
    Arrow,
    Wave,
    Pen,
    Shape,
};
struct AnnotToolDetailDescriptor {
    ToolMode mode = ToolMode::Select;
    AnnotToolFamily family = AnnotToolFamily::Select;
    const char* key = "select";
    AnnotToolOptionGroup optionGroup = AnnotToolOptionGroup::None;
    AnnotToolGeometry geometry = AnnotToolGeometry::None;
    AnnotToolPresentation presentation = AnnotToolPresentation::None;
};

// Pure conversions: no UI, mutable selection state, or persistence dependencies.
// Descriptor pointers and referenced orders remain valid for the process lifetime.
// FromKey returns false without changing out for unknown keys.
[[nodiscard]] std::vector<ToolMode> DefaultAnnotToolModeUiOrder();
[[nodiscard]] const std::vector<AnnotToolFamily>& AnnotToolFamilyUiOrder();
[[nodiscard]] const std::vector<AnnotToolDetailDescriptor>& AnnotToolDetailDescriptors();
[[nodiscard]] const AnnotToolDetailDescriptor* FindAnnotToolDetailDescriptor(ToolMode mode);
[[nodiscard]] AnnotToolGeometry AnnotToolGeometryForMode(ToolMode mode);
[[nodiscard]] AnnotToolPresentation AnnotToolPresentationForMode(ToolMode mode, ShapeDrawMode shapeDrawMode);
[[nodiscard]] ShapeToolSelection ShapeToolSelectionForMode(ToolMode mode, ShapeDrawMode shapeDrawMode);
[[nodiscard]] std::optional<ToolMode> ToolModeForShapeToolSelection(ShapeToolSelection selection);
[[nodiscard]] const char* ShapeToolPresentationKey(AnnotToolPresentation presentation);
[[nodiscard]] bool ShapeToolPresentationFromKey(const std::string& key, AnnotToolPresentation& out);
[[nodiscard]] const char* ShapeToolGeometryKey(AnnotToolGeometry geometry);
[[nodiscard]] bool ShapeToolGeometryFromKey(const std::string& key, AnnotToolGeometry& out);
[[nodiscard]] const char* ShapeDetailKey(ShapeDetail detail);
[[nodiscard]] bool ShapeDetailFromKey(const std::string& key, ShapeDetail& out);
[[nodiscard]] std::vector<ShapeDetail> OrderedShapeDetails();
[[nodiscard]] bool ShapeDetailIsLinear(ShapeDetail detail);
[[nodiscard]] bool ShapeDetailUsesBox(ShapeDetail detail);
[[nodiscard]] ToolMode ToolModeForShapeDetail(ShapeDetail detail);
[[nodiscard]] ShapeKind ShapeKindForShapeDetail(ShapeDetail detail);
[[nodiscard]] ShapeDetail ShapeDetailForLegacyState(ToolMode mode, ShapeKind kind);
[[nodiscard]] bool AnnotToolUsesGeometry(ToolMode mode, AnnotToolGeometry geometry);
[[nodiscard]] bool IsLineLikeAnnotToolMode(ToolMode mode);
[[nodiscard]] bool IsArrowAnnotToolMode(ToolMode mode);
[[nodiscard]] bool IsWaveAnnotToolMode(ToolMode mode);
[[nodiscard]] bool IsEmphasisAnnotToolMode(ToolMode mode, ShapeDrawMode shapeDrawMode);
[[nodiscard]] const char* AnnotToolFamilyKey(AnnotToolFamily family);
[[nodiscard]] bool AnnotToolFamilyFromKey(const std::string& key, AnnotToolFamily& out);
[[nodiscard]] const char* AnnotToolModeKey(ToolMode mode);
[[nodiscard]] bool AnnotToolModeFromKey(const std::string& key, ToolMode& out);
[[nodiscard]] AnnotToolFamily AnnotToolFamilyForMode(ToolMode mode);
