#include "readonly_viewer/mermaid_subset_parser.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <utility>

namespace textviewer::mermaid {
namespace {

struct ParsedEndpoint {
    std::wstring id;
    std::wstring label;
    NodeShape shape = NodeShape::Rectangle;
};

struct ParsedEdgeOperator {
    size_t offset = 0;
    size_t length = 0;
    EdgeStyle style = EdgeStyle::SolidArrow;
};

bool IsIdentifierStart(wchar_t value) {
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z') || value == L'_';
}

bool IsIdentifierCharacter(wchar_t value) {
    return IsIdentifierStart(value) || (value >= L'0' && value <= L'9') || value == L'-';
}

std::wstring_view Trim(std::wstring_view value) {
    while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
    while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
    return value;
}

bool IsCommentOrEmpty(std::wstring_view value) {
    value = Trim(value);
    return value.empty() || (value.size() >= 2 && value[0] == L'%' && value[1] == L'%');
}

bool ParseDirection(std::wstring_view value, FlowDirection* out_direction) {
    const size_t separator = value.find_first_of(L" \t");
    const std::wstring_view keyword = value.substr(0, separator);
    if (keyword != L"flowchart" && keyword != L"graph") return false;
    value = separator == std::wstring_view::npos ? std::wstring_view{} : Trim(value.substr(separator + 1));
    if (value == L"TD" || value == L"TB") *out_direction = FlowDirection::TopDown;
    else if (value == L"LR") *out_direction = FlowDirection::LeftRight;
    else if (value == L"BT") *out_direction = FlowDirection::BottomTop;
    else if (value == L"RL") *out_direction = FlowDirection::RightLeft;
    else return false;
    return true;
}

std::optional<ParsedEdgeOperator> FindEdgeOperator(std::wstring_view value) {
    for (size_t i = 0; i < value.size(); ++i) {
        if (value.substr(i, 4) == L"-.->") return ParsedEdgeOperator{i, 4, EdgeStyle::DottedArrow};
        if (value.substr(i, 3) == L"==>") return ParsedEdgeOperator{i, 3, EdgeStyle::ThickArrow};
        if (value.substr(i, 3) == L"-->") return ParsedEdgeOperator{i, 3, EdgeStyle::SolidArrow};
        if (value.substr(i, 3) == L"---") return ParsedEdgeOperator{i, 3, EdgeStyle::SolidLine};
    }
    return std::nullopt;
}

bool ParseEndpoint(std::wstring_view value, const ParseLimits& limits, ParsedEndpoint* out_endpoint) {
    value = Trim(value);
    if (value.empty() || !IsIdentifierStart(value.front())) return false;

    size_t id_end = 1;
    while (id_end < value.size() && IsIdentifierCharacter(value[id_end])) ++id_end;
    ParsedEndpoint endpoint;
    endpoint.id.assign(value.substr(0, id_end));

    const std::wstring_view suffix = Trim(value.substr(id_end));
    if (suffix.empty()) {
        endpoint.label = endpoint.id;
        *out_endpoint = std::move(endpoint);
        return true;
    }

    wchar_t close = 0;
    if (suffix.front() == L'[') {
        endpoint.shape = NodeShape::Rectangle;
        close = L']';
    } else if (suffix.front() == L'(') {
        endpoint.shape = NodeShape::RoundedRectangle;
        close = L')';
    } else if (suffix.front() == L'{') {
        endpoint.shape = NodeShape::Diamond;
        close = L'}';
    } else {
        return false;
    }
    if (suffix.size() < 2 || suffix.back() != close) return false;

    const std::wstring_view label = suffix.substr(1, suffix.size() - 2);
    if (label.size() > limits.max_label_characters) return false;
    endpoint.label.assign(label);
    *out_endpoint = std::move(endpoint);
    return true;
}

std::optional<size_t> FindNode(const DiagramModel& model, std::wstring_view id) {
    for (size_t index = 0; index < model.nodes.size(); ++index) {
        if (model.nodes[index].id == id) return index;
    }
    return std::nullopt;
}

bool AddOrUpdateNode(DiagramModel* model, ParsedEndpoint endpoint, size_t group_index,
                     const ParseLimits& limits, size_t* out_index) {
    if (const auto existing = FindNode(*model, endpoint.id)) {
        DiagramNode& node = model->nodes[*existing];
        if (endpoint.label != endpoint.id) {
            node.label = std::move(endpoint.label);
            node.shape = endpoint.shape;
        }
        if (node.group_index == kNoDiagramGroup) node.group_index = group_index;
        *out_index = *existing;
        return true;
    }
    if (model->nodes.size() >= limits.max_nodes) return false;
    model->nodes.push_back({std::move(endpoint.id), std::move(endpoint.label), endpoint.shape, group_index});
    *out_index = model->nodes.size() - 1;
    return true;
}

bool ParseEdge(std::wstring_view line, DiagramModel* model, size_t group_index, const ParseLimits& limits) {
    const auto direct_operator = FindEdgeOperator(line);
    ParsedEdgeOperator edge_operator;
    std::wstring label;
    std::wstring_view destination_source;
    if (direct_operator) {
        edge_operator = *direct_operator;
        destination_source = Trim(line.substr(edge_operator.offset + edge_operator.length));
    } else {
        const size_t dotted_start = line.find(L"-.");
        if (dotted_start == std::wstring_view::npos) return false;
        const size_t dotted_end = line.find(L".->", dotted_start + 2);
        if (dotted_end == std::wstring_view::npos) return false;
        const std::wstring_view dotted_label = Trim(line.substr(dotted_start + 2, dotted_end - dotted_start - 2));
        if (dotted_label.empty() || dotted_label.size() > limits.max_label_characters) return false;
        edge_operator = {dotted_start, 2, EdgeStyle::DottedArrow};
        label.assign(dotted_label);
        destination_source = Trim(line.substr(dotted_end + 3));
    }

    ParsedEndpoint from;
    if (!ParseEndpoint(line.substr(0, edge_operator.offset), limits, &from)) return false;

    if (label.empty() && !destination_source.empty() && destination_source.front() == L'|') {
        const size_t label_end = destination_source.find(L'|', 1);
        if (label_end == std::wstring_view::npos || label_end - 1 > limits.max_label_characters) return false;
        label.assign(destination_source.substr(1, label_end - 1));
        destination_source = Trim(destination_source.substr(label_end + 1));
    }

    ParsedEndpoint to;
    if (!ParseEndpoint(destination_source, limits, &to)) return false;
    if (model->edges.size() >= limits.max_edges) return false;

    size_t from_index = 0;
    size_t to_index = 0;
    if (!AddOrUpdateNode(model, std::move(from), group_index, limits, &from_index) ||
        !AddOrUpdateNode(model, std::move(to), group_index, limits, &to_index)) {
        return false;
    }
    model->edges.push_back({from_index, to_index, std::move(label), edge_operator.style});
    return true;
}

bool ParseNodeDeclaration(std::wstring_view line, DiagramModel* model, size_t group_index, const ParseLimits& limits) {
    ParsedEndpoint endpoint;
    if (!ParseEndpoint(line, limits, &endpoint)) return false;
    size_t node_index = 0;
    return AddOrUpdateNode(model, std::move(endpoint), group_index, limits, &node_index);
}

bool ParseSubgraphDeclaration(std::wstring_view line, DiagramModel* model, std::vector<size_t>* group_stack,
                              const ParseLimits& limits) {
    line = Trim(line);
    constexpr std::wstring_view prefix = L"subgraph";
    if (line.size() <= prefix.size() || line.substr(0, prefix.size()) != prefix ||
        !iswspace(line[prefix.size()])) return false;
    if (model->groups.size() >= limits.max_groups) return false;
    std::wstring_view label = Trim(line.substr(prefix.size()));
    const size_t bracket = label.find(L'[');
    if (bracket != std::wstring_view::npos && label.back() == L']') {
        label = label.substr(bracket + 1, label.size() - bracket - 2);
    }
    if (label.empty() || label.size() > limits.max_label_characters) return false;
    const size_t parent = group_stack->empty() ? kNoDiagramGroup : group_stack->back();
    model->groups.push_back({std::wstring(label), parent});
    group_stack->push_back(model->groups.size() - 1);
    return true;
}

void AddDiagnostic(ParseResult* result, size_t line_number, std::wstring message) {
    result->diagnostics.push_back({line_number, std::move(message)});
}

} // namespace

ParseResult ParseFlowchart(std::wstring_view source, const ParseLimits& limits) {
    ParseResult result;
    if (source.size() > limits.max_source_characters) {
        AddDiagnostic(&result, 0, L"Mermaid block exceeds the supported size limit.");
        return result;
    }

    bool found_header = false;
    std::vector<size_t> group_stack;
    size_t line_number = 0;
    size_t offset = 0;
    while (offset <= source.size()) {
        const size_t line_end = source.find(L'\n', offset);
        std::wstring_view line = source.substr(offset, line_end == std::wstring_view::npos ? source.size() - offset : line_end - offset);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        ++line_number;
        if (line_number > limits.max_lines) {
            AddDiagnostic(&result, line_number, L"Mermaid block exceeds the supported line limit.");
            return result;
        }

        if (!IsCommentOrEmpty(line)) {
            if (!found_header) {
                if (!ParseDirection(Trim(line), &result.model.direction)) {
                    AddDiagnostic(&result, line_number, L"Expected a supported flowchart or graph direction declaration.");
                    return result;
                }
                found_header = true;
            } else if (Trim(line) == L"end") {
                if (group_stack.empty()) {
                    AddDiagnostic(&result, line_number, L"Mermaid subgraph end does not have a matching subgraph.");
                    return result;
                }
                group_stack.pop_back();
            } else if (!ParseSubgraphDeclaration(line, &result.model, &group_stack, limits) &&
                       !ParseEdge(line, &result.model, group_stack.empty() ? kNoDiagramGroup : group_stack.back(), limits) &&
                       !ParseNodeDeclaration(line, &result.model, group_stack.empty() ? kNoDiagramGroup : group_stack.back(), limits)) {
                AddDiagnostic(&result, line_number, L"Unsupported Mermaid flowchart statement.");
                return result;
            }
        }

        if (line_end == std::wstring_view::npos) break;
        offset = line_end + 1;
    }

    if (!found_header) {
        AddDiagnostic(&result, 0, L"Mermaid block does not contain a flowchart declaration.");
    } else if (result.model.nodes.empty()) {
        AddDiagnostic(&result, 0, L"Mermaid flowchart does not contain any nodes.");
    } else if (!group_stack.empty()) {
        AddDiagnostic(&result, line_number, L"Mermaid subgraph does not have a matching end.");
    }
    return result;
}

} // namespace textviewer::mermaid
