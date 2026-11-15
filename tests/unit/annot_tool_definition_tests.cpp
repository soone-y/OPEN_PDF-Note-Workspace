#include "core/annot_tool_definition.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void Check(bool value, const std::string& context) {
    ++checks;
    if (!value) throw std::runtime_error(context);
}

template<class T, class Parser>
void CheckKeys(Parser parse, const std::vector<std::pair<const char*, T>>& keys, T sentinel) {
    for (const auto& entry : keys) {
        T value = sentinel;
        Check(parse(entry.first, value) && value == entry.second, entry.first);
        std::string upper = entry.first;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        Check(parse(upper, value) && value == entry.second, upper);
    }
    for (const auto* unknown : {"", "unknown", " line", "line ", "\xff"}) {
        T value = sentinel;
        Check(!parse(unknown, value) && value == sentinel, "unknown key preserves output");
    }
}

void CheckTools() {
    using M = ToolMode;
    using F = AnnotToolFamily;
    using G = AnnotToolGeometry;
    using P = AnnotToolPresentation;
    using O = AnnotToolOptionGroup;
    const std::vector<AnnotToolDetailDescriptor> expected = {
        {M::Select, F::Select, "select", O::None, G::None, P::None},
        {M::Pan, F::Pan, "pan", O::None, G::None, P::None},
        {M::Magnifier, F::Magnifier, "magnifier", O::Magnifier, G::None, P::None},
        {M::TextBox, F::Text, "text", O::Text, G::None, P::None},
        {M::MarkerText, F::Marker, "marker_text", O::MarkerText, G::None, P::None},
        {M::MarkerTextUnderline, F::Marker, "marker_text_underline", O::MarkerText, G::None, P::None},
        {M::MarkerTextColor, F::Marker, "marker_text_color", O::MarkerText, G::None, P::None},
        {M::MarkerFree, F::Marker, "marker_free", O::MarkerFree, G::None, P::None},
        {M::Freehand, F::Pen, "freehand", O::Pen, G::None, P::None},
        {M::Line, F::Shape, "line", O::Line, G::Line, P::Stroke},
        {M::Wave, F::Shape, "wave", O::Wave, G::Wave, P::Stroke},
        {M::Arrow, F::Shape, "arrow", O::Arrow, G::Arrow, P::Stroke},
        {M::Shape, F::Shape, "shape", O::Shape, G::Shape, P::None},
        {M::MarkerLine, F::Shape, "marker_line", O::Line, G::Line, P::Emphasis},
        {M::MarkerWave, F::Shape, "marker_wave", O::Wave, G::Wave, P::Emphasis},
        {M::MarkerArrow, F::Shape, "marker_arrow", O::Arrow, G::Arrow, P::Emphasis},
        {M::Eraser, F::Eraser, "eraser", O::None, G::None, P::None},
    };
    auto order = DefaultAnnotToolModeUiOrder();
    const auto& actual = AnnotToolDetailDescriptors();
    Check(order.size() == expected.size() && actual.size() == expected.size(), "all tools present");
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        const auto& a = actual[i];
        Check(order[i] == e.mode && a.mode == e.mode && a.family == e.family &&
              std::string(a.key) == e.key && a.optionGroup == e.optionGroup &&
              a.geometry == e.geometry && a.presentation == e.presentation, e.key);
        Check(FindAnnotToolDetailDescriptor(e.mode) == &actual[i], "stable descriptor address");
        Check(AnnotToolFamilyForMode(e.mode) == e.family, "family");
        Check(AnnotToolGeometryForMode(e.mode) == e.geometry, "geometry");
        Check(std::string(AnnotToolModeKey(e.mode)) == e.key, "canonical tool key");
        CheckKeys<M>(AnnotToolModeFromKey, {{e.key, e.mode}}, M::Eraser);
        for (auto draw : {ShapeDrawMode::Outline, ShapeDrawMode::Fill, ShapeDrawMode::Line}) {
            const auto presentation = e.mode == M::Shape ?
                (draw == ShapeDrawMode::Fill ? P::Emphasis : P::Stroke) : e.presentation;
            Check(AnnotToolPresentationForMode(e.mode, draw) == presentation, "presentation");
            const ShapeToolSelection selection{presentation, e.geometry};
            Check(ShapeToolSelectionForMode(e.mode, draw) == selection, "selection");
            const auto mode = ToolModeForShapeToolSelection(selection);
            Check(e.geometry == G::None ? !mode : mode == e.mode, "shape selection round trip");
            Check(IsEmphasisAnnotToolMode(e.mode, draw) == (presentation == P::Emphasis), "emphasis");
        }
        Check(IsLineLikeAnnotToolMode(e.mode) == (e.geometry == G::Line || e.geometry == G::Wave || e.geometry == G::Arrow), "linear");
        Check(IsArrowAnnotToolMode(e.mode) == (e.geometry == G::Arrow), "arrow");
        Check(IsWaveAnnotToolMode(e.mode) == (e.geometry == G::Wave), "wave");
        for (auto geometry : {G::None, G::Shape, G::Line, G::Wave, G::Arrow})
            Check(AnnotToolUsesGeometry(e.mode, geometry) == (geometry != G::None && geometry == e.geometry), "uses geometry");
    }
    order.clear();
    Check(DefaultAnnotToolModeUiOrder().size() == expected.size(), "caller owns default order copy");
    CheckKeys<M>(AnnotToolModeFromKey, {
        {"markertext", M::MarkerText}, {"markertextunderline", M::MarkerTextUnderline},
        {"marker_underline", M::MarkerTextUnderline}, {"markerunderline", M::MarkerTextUnderline},
        {"markertextcolor", M::MarkerTextColor}, {"marker_color_text", M::MarkerTextColor},
        {"markercolortext", M::MarkerTextColor}, {"text_color_marker", M::MarkerTextColor},
        {"textcolormarker", M::MarkerTextColor}, {"markerfree", M::MarkerFree},
        {"markerline", M::MarkerLine}, {"markerarrow", M::MarkerArrow},
        {"markerwave", M::MarkerWave}, {"textbox", M::TextBox}}, M::Eraser);
    const std::vector<F> families{F::Select, F::Pan, F::Magnifier, F::Text, F::Marker, F::Pen, F::Shape, F::Eraser};
    Check(AnnotToolFamilyUiOrder() == families, "family order");
    const char* familyKeys[]{"select", "pan", "magnifier", "text", "marker", "pen", "shape", "eraser"};
    for (size_t i = 0; i < families.size(); ++i) {
        Check(std::string(AnnotToolFamilyKey(families[i])) == familyKeys[i], "family key");
        CheckKeys<F>(AnnotToolFamilyFromKey, {{familyKeys[i], families[i]}}, F::Eraser);
    }
    CheckKeys<F>(AnnotToolFamilyFromKey, {{"textbox", F::Text}, {"freehand", F::Pen}}, F::Eraser);
    for (int invalid : {-1, kToolModeCount, 999}) {
        const auto mode = static_cast<M>(invalid);
        Check(!FindAnnotToolDetailDescriptor(mode), "unknown descriptor");
        Check(std::string(AnnotToolModeKey(mode)).empty(), "unknown tool key");
        Check(AnnotToolFamilyForMode(mode) == F::Eraser, "legacy unknown family fallback");
        Check(AnnotToolGeometryForMode(mode) == G::None, "unknown geometry");
        Check(AnnotToolPresentationForMode(mode, ShapeDrawMode::Fill) == P::None, "unknown presentation");
    }
    Check(std::string(AnnotToolFamilyKey(static_cast<F>(999))).empty(), "unknown family key");
    Check(!ToolModeForShapeToolSelection({P::None, G::Line}), "invalid selection presentation");
    Check(!ToolModeForShapeToolSelection({P::Stroke, static_cast<G>(999)}), "invalid selection geometry");
}

void CheckShapes() {
    using D = ShapeDetail;
    using K = ShapeKind;
    using M = ToolMode;
    using P = AnnotToolPresentation;
    using G = AnnotToolGeometry;
    const std::vector<D> details{D::Line, D::Arrow, D::Wave, D::Rectangle, D::Ellipse, D::Triangle, D::Diamond, D::Arc, D::Star};
    const char* keys[]{"line", "arrow", "wave", "rectangle", "ellipse", "triangle", "diamond", "arc", "star"};
    Check(OrderedShapeDetails() == details, "shape detail order");
    for (size_t i = 0; i < details.size(); ++i) {
        Check(std::string(ShapeDetailKey(details[i])) == keys[i], "shape detail key");
        CheckKeys<D>(ShapeDetailFromKey, {{keys[i], details[i]}}, D::Star);
        Check(ShapeDetailIsLinear(details[i]) == (i < 3), "linear detail");
        Check(ShapeDetailUsesBox(details[i]) == (i >= 3), "box detail");
        Check(ShapeDetailForLegacyState(ToolModeForShapeDetail(details[i]), ShapeKindForShapeDetail(details[i])) == details[i], "detail round trip");
    }
    CheckKeys<D>(ShapeDetailFromKey, {{"marker_line", D::Line}, {"marker_arrow", D::Arrow},
        {"marker_wave", D::Wave}, {"shape", D::Rectangle}, {"rect", D::Rectangle},
        {"square", D::Rectangle}, {"circle", D::Ellipse}, {"rotated_ellipse", D::Ellipse},
        {"equilateral_triangle", D::Triangle}}, D::Star);
    for (auto kind : {K::Square, K::Rectangle, K::Diamond, K::EquilateralTriangle, K::Triangle,
                      K::Ellipse, K::Circle, K::RotatedEllipse, K::Arc, K::Star}) {
        const D expected[]{D::Rectangle, D::Rectangle, D::Diamond, D::Triangle, D::Triangle,
                           D::Ellipse, D::Ellipse, D::Ellipse, D::Arc, D::Star};
        Check(ShapeDetailForLegacyState(M::Shape, kind) == expected[static_cast<int>(kind)], "legacy kind");
        Check(ShapeDetailForLegacyState(M::MarkerLine, kind) == D::Line, "legacy marker line");
        Check(ShapeDetailForLegacyState(M::MarkerArrow, kind) == D::Arrow, "legacy marker arrow");
        Check(ShapeDetailForLegacyState(M::MarkerWave, kind) == D::Wave, "legacy marker wave");
    }
    CheckKeys<P>(ShapeToolPresentationFromKey, {{"stroke", P::Stroke}, {"emphasis", P::Emphasis}}, P::None);
    CheckKeys<G>(ShapeToolGeometryFromKey, {{"line", G::Line}, {"wave", G::Wave}, {"arrow", G::Arrow}, {"shape", G::Shape}}, G::None);
    Check(std::string(ShapeToolPresentationKey(P::Stroke)) == "stroke" &&
          std::string(ShapeToolPresentationKey(P::Emphasis)) == "emphasis", "presentation keys");
    for (auto entry : std::vector<std::pair<G, const char*>>{{G::Line, "line"}, {G::Wave, "wave"}, {G::Arrow, "arrow"}, {G::Shape, "shape"}})
        Check(std::string(ShapeToolGeometryKey(entry.first)) == entry.second, "geometry keys");
    Check(std::string(ShapeToolPresentationKey(P::None)) == "stroke", "legacy presentation fallback");
    Check(std::string(ShapeToolGeometryKey(G::None)) == "line", "legacy geometry fallback");
    const auto invalid = static_cast<D>(999);
    Check(std::string(ShapeDetailKey(invalid)) == "line", "legacy detail key fallback");
    Check(!ShapeDetailIsLinear(invalid) && ShapeDetailUsesBox(invalid), "legacy unknown box");
    Check(ToolModeForShapeDetail(invalid) == M::Line && ShapeKindForShapeDetail(invalid) == K::Rectangle, "legacy detail fallback");
    Check(ShapeDetailForLegacyState(M::Shape, static_cast<K>(999)) == D::Rectangle, "unknown legacy kind");
    Check(ShapeDetailForLegacyState(M::Select, K::Star) == D::Line, "non-shape fallback");
}
}

int main() {
    try {
        CheckTools();
        CheckShapes();
        std::cout << "PASS annotation tool definitions: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL check " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
