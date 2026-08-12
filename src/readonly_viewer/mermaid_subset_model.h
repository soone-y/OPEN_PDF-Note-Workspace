#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace textviewer::mermaid {

enum class FlowDirection {
    TopDown,
    LeftRight,
    BottomTop,
    RightLeft,
};

enum class NodeShape {
    Rectangle,
    RoundedRectangle,
    Diamond,
};

enum class EdgeStyle {
    SolidArrow,
    SolidLine,
    DottedArrow,
    ThickArrow,
};

constexpr size_t kNoDiagramGroup = std::numeric_limits<size_t>::max();

struct DiagramNode {
    std::wstring id;
    std::wstring label;
    NodeShape shape = NodeShape::Rectangle;
    size_t group_index = kNoDiagramGroup;
};

struct DiagramGroup {
    std::wstring label;
    size_t parent_group = kNoDiagramGroup;
};

struct DiagramEdge {
    size_t from_node = 0;
    size_t to_node = 0;
    std::wstring label;
    EdgeStyle style = EdgeStyle::SolidArrow;
};

struct DiagramModel {
    FlowDirection direction = FlowDirection::TopDown;
    std::vector<DiagramNode> nodes;
    std::vector<DiagramEdge> edges;
    std::vector<DiagramGroup> groups;
};

struct ParseLimits {
    size_t max_source_characters = 64 * 1024;
    size_t max_lines = 1024;
    size_t max_nodes = 256;
    size_t max_edges = 512;
    size_t max_groups = 64;
    size_t max_label_characters = 1024;
};

struct ParseDiagnostic {
    size_t line_number = 0;
    std::wstring message;
};

struct ParseResult {
    DiagramModel model;
    std::vector<ParseDiagnostic> diagnostics;

    [[nodiscard]] bool can_render() const noexcept {
        return diagnostics.empty() && !model.nodes.empty();
    }
};

} // namespace textviewer::mermaid
