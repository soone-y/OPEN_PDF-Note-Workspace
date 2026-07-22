#include <iostream>
#include <string>

#include "readonly_viewer/mermaid_subset_parser.h"

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failures;
}

} // namespace

int main() {
    using namespace textviewer::mermaid;

    {
        const ParseResult result = ParseFlowchart(
            L"flowchart TD\n"
            L"subgraph First[最初の段階]\n"
            L"  A[開始] --> B[処理]\n"
            L"  subgraph Inner[詳細]\n"
            L"    B --> C[完了]\n"
            L"  end\n"
            L"end\n");
        Expect(result.can_render() && result.model.groups.size() == 2,
               "nested subgraph declarations are parsed");
        Expect(result.model.nodes[0].group_index == 0 && result.model.nodes[2].group_index == 1,
               "nodes retain their active subgraph membership");
    }

    {
        const ParseResult result = ParseFlowchart(
            L"flowchart LR\n"
            L"    A[開始] -->|準備| B(処理)\n"
            L"    B -.-> C{完了?}\n");
        Expect(result.can_render(), "basic Japanese flowchart can be parsed");
        Expect(result.model.direction == FlowDirection::LeftRight,
               "flowchart direction is preserved");
        Expect(result.model.nodes.size() == 3, "nodes are collected from edge endpoints");
        Expect(result.model.edges.size() == 2, "edges are collected");
        Expect(result.model.nodes[1].shape == NodeShape::RoundedRectangle,
               "rounded node shape is parsed");
        Expect(result.model.nodes[2].shape == NodeShape::Diamond,
               "diamond node shape is parsed");
        Expect(result.model.edges[0].label == L"準備", "edge label is parsed");
        Expect(result.model.edges[1].style == EdgeStyle::DottedArrow,
               "dotted edge style is parsed");
    }

    {
        const ParseResult result = ParseFlowchart(
            L"%% comment\n"
            L"graph TD\n"
            L"A --> B\n"
            L"B ==> C\n"
            L"C --- D\n"
            L"D -. 任意 .-> E\n");
        Expect(result.can_render(), "graph alias and comments are accepted");
        Expect(result.model.edges[1].style == EdgeStyle::ThickArrow,
               "thick arrow is parsed");
        Expect(result.model.edges[2].style == EdgeStyle::SolidLine,
               "solid line is parsed");
        Expect(result.model.edges[3].label == L"任意",
               "dotted edge text form is parsed");
    }

    {
        const ParseResult result = ParseFlowchart(
            L"flowchart LR\n"
            L"    Z[通常版またはLite版のZIP] --> E[書き込み可能な場所へ展開]\n"
            L"    E --> A[pdf_note_workspace.exe]\n"
            L"    E -. 任意 .-> S[ショートカットを作成]\n"
            L"    E -. 任意 .-> O[Windows の「プログラムから開く」で選択]\n"
            L"    W[個人のワークスペース] -. 別の場所に保管 .-> A\n");
        Expect(result.can_render(), "setup guide flowchart is parsed");
        Expect(result.model.nodes.size() == 6 && result.model.edges.size() == 5,
               "setup guide flowchart retains all nodes and edges");
    }

    {
        const ParseResult result = ParseFlowchart(L"flowchart TD\nStandalone[独立ノード]\n");
        Expect(result.can_render() && result.model.nodes.size() == 1,
               "standalone node declaration is parsed");
    }

    {
        const ParseResult result = ParseFlowchart(
            L"flowchart TD\n"
            L"Long[長いラベルでも折り返して表示できることを確認するための十分に長い日本語のノード名]\n"
            L"Long --> Next[次の処理]\n"
            L"Next --> Long\n"
            L"Next --> Next\n");
        Expect(result.can_render(), "long labels with cyclic and self-loop edges are accepted");
        Expect(result.model.nodes.size() == 2 && result.model.edges.size() == 3,
               "cyclic and self-loop edges are retained");
    }

    {
        const ParseResult result = ParseFlowchart(
            L"flowchart TD\n"
            L"subgraph MissingEnd\n"
            L"A --> B\n");
        Expect(!result.can_render() && !result.diagnostics.empty(),
               "unclosed subgraph reports a safe parse failure");
    }

    {
        const ParseResult result = ParseFlowchart(L"sequenceDiagram\nA->>B: hello\n");
        Expect(!result.can_render() && !result.diagnostics.empty(),
               "unsupported diagram type reports a safe parse failure");
    }

    {
        ParseLimits limits;
        limits.max_nodes = 2;
        const ParseResult result = ParseFlowchart(L"flowchart TD\nA --> B\nB --> C\n", limits);
        Expect(!result.can_render() && !result.diagnostics.empty(),
               "node limit prevents unbounded graph allocation");
    }

    return g_failures == 0 ? 0 : 1;
}
