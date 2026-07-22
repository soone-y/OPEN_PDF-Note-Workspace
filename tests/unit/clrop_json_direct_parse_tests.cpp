#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "clrop/json.h"

namespace {

namespace fs = std::filesystem;

int g_failed = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failed;
}

struct SinkRecord {
    int page = 0;
    clrop::Item item;
};

class CollectSink final : public clrop::ItemLoadSink {
public:
    bool OnClropItem(int page, clrop::Item&& item, std::wstring& err) override {
        (void)err;
        records.push_back(SinkRecord{ page, std::move(item) });
        return true;
    }

    std::vector<SinkRecord> records;
};

fs::path TempClropRoot() {
    fs::path root = fs::temp_directory_path() / L"pdf_note_clrop_json_tests";
    static const std::wstring unique =
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    return root / unique;
}

fs::path MakeTempClropPath(const wchar_t* leaf) {
    fs::path root = TempClropRoot();
    std::error_code ec;
    fs::create_directories(root, ec);
    return root / leaf;
}

bool WriteRaw(const fs::path& path, const std::string& contents) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(ofs);
}

bool Near(double a, double b) {
    return std::abs(a - b) < 0.0000001;
}

bool SameArray2(const std::optional<std::array<double, 2>>& lhs,
                const std::optional<std::array<double, 2>>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    if (!lhs.has_value()) return true;
    return Near((*lhs)[0], (*rhs)[0]) && Near((*lhs)[1], (*rhs)[1]);
}

bool SameArray4(const std::optional<std::array<double, 4>>& lhs,
                const std::optional<std::array<double, 4>>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    if (!lhs.has_value()) return true;
    return Near((*lhs)[0], (*rhs)[0]) && Near((*lhs)[1], (*rhs)[1]) &&
           Near((*lhs)[2], (*rhs)[2]) && Near((*lhs)[3], (*rhs)[3]);
}

bool SameDoubleVector(const std::vector<double>& lhs, const std::vector<double>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!Near(lhs[i], rhs[i])) return false;
    }
    return true;
}

bool SameOptionalDouble(const std::optional<double>& lhs, const std::optional<double>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    if (!lhs) return true;
    return Near(*lhs, *rhs);
}

clrop::Item MakeFullRoundTripItem(clrop::Item::Kind kind, const wchar_t* id, double offset) {
    clrop::Item item;
    item.kind = kind;
    item.id = id;
    item.created = L"2026-05-11T00:00:00Z";
    item.updated = L"2026-05-11T00:00:01Z";
    item.color = "#123456";
    item.alpha = 0.625;
    item.width = 1.25 + offset;
    item.linkId = L"link-target";
    item.notePath = L"lecture/session/note.md";

    switch (kind) {
    case clrop::Item::Kind::Text:
        item.content = L"text body";
        item.lines = {L"line 1", L"line 2"};
        item.bbox = std::array<double, 4>{1.25 + offset, 2.5, 30.75, 40.125};
        item.pt = 11.5;
        item.font = L"Yu Gothic";
        item.border = "rect";
        break;
    case clrop::Item::Kind::Math:
        item.content = L"x^2 + y^2";
        item.mathKind = "latex";
        item.bbox = std::array<double, 4>{3.25 + offset, 4.5, 50.75, 60.125};
        item.pt = 13.0;
        item.font = L"Cambria Math";
        item.border = "none";
        break;
    case clrop::Item::Kind::MarkerText:
    case clrop::Item::Kind::TextColor:
        item.quads = {
            1.0 + offset, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
            9.0 + offset, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0
        };
        break;
    case clrop::Item::Kind::MarkerFree:
        item.path = {1.0 + offset, 2.0, 3.0, 4.0, 5.0, 6.0};
        break;
    case clrop::Item::Kind::Line:
    case clrop::Item::Kind::Wave:
        item.p1 = std::array<double, 2>{10.0 + offset, 20.0};
        item.p2 = std::array<double, 2>{30.0 + offset, 40.0};
        item.dash = {2.0, 1.0, 0.5};
        break;
    case clrop::Item::Kind::Arrow:
        item.p1 = std::array<double, 2>{10.0 + offset, 20.0};
        item.p2 = std::array<double, 2>{30.0 + offset, 40.0};
        item.dash = {2.0, 1.0, 0.5};
        item.arrowHead = "double";
        break;
    case clrop::Item::Kind::Freehand:
        item.path = {10.0 + offset, 20.0, 30.0, 40.0, 50.0, 60.0};
        break;
    case clrop::Item::Kind::LinkMarker:
        item.p1 = std::array<double, 2>{5.0 + offset, 6.0};
        break;
    case clrop::Item::Kind::Shape:
        item.bbox = std::array<double, 4>{7.0 + offset, 8.0, 90.0, 100.0};
        item.shapeKind = "rotated_ellipse";
        item.shapeDrawMode = "outline";
        item.shapeRotation = 0.37;
        break;
    }
    return item;
}

bool SameRoundTripItem(const clrop::Item& expected, const clrop::Item& actual) {
    return expected.kind == actual.kind &&
           expected.id == actual.id &&
           expected.created == actual.created &&
           expected.updated == actual.updated &&
           expected.color == actual.color &&
           Near(expected.alpha, actual.alpha) &&
           Near(expected.width, actual.width) &&
           Near(expected.pt, actual.pt) &&
           expected.font == actual.font &&
           expected.border == actual.border &&
           expected.content == actual.content &&
           expected.lines == actual.lines &&
           expected.linkId == actual.linkId &&
           expected.notePath == actual.notePath &&
           expected.mathKind == actual.mathKind &&
           expected.shapeKind == actual.shapeKind &&
           expected.shapeDrawMode == actual.shapeDrawMode &&
           expected.arrowHead == actual.arrowHead &&
           SameOptionalDouble(expected.shapeRotation, actual.shapeRotation) &&
           SameArray4(expected.bbox, actual.bbox) &&
           SameArray2(expected.p1, actual.p1) &&
           SameArray2(expected.p2, actual.p2) &&
           SameDoubleVector(expected.quads, actual.quads) &&
           SameDoubleVector(expected.path, actual.path) &&
           SameDoubleVector(expected.dash, actual.dash);
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    {
        std::error_code ec;
        fs::remove_all(TempClropRoot(), ec);
    }

    {
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{"
                "\"path\":\"\\u65e5\\u672c.pdf\","
                "\"size\":42,"
                "\"page_sizes_pt\":[[595,842],[612,792]],"
                "\"sha256\":\"abc123\""
            "},"
            "\"pages\":["
                "{"
                    "\"page\":3,"
                    "\"items\":["
                        "{"
                            "\"type\":\"text\","
                            "\"id\":\"text-1\","
                            "\"created\":\"2026-04-27T21:30:00Z\","
                            "\"updated\":\"2026-04-27T21:31:00Z\","
                            "\"content\":\"hello\","
                            "\"lines\":[\"a\",\"b\"],"
                            "\"bbox\":[1,2,3,4],"
                            "\"pt\":10,"
                            "\"font\":\"Yu Gothic\","
                            "\"color\":\"#112233\""
                        "},"
                        "{"
                            "\"type\":\"marker-free\","
                            "\"id\":\"marker-1\","
                            "\"created\":\"2026-04-27T21:30:00Z\","
                            "\"updated\":\"2026-04-27T21:31:00Z\","
                            "\"path\":[[1,2],[3,4]],"
                            "\"width\":5,"
                            "\"alpha\":0.7,"
                            "\"color\":\"#abcdef\""
                        "},"
                        "{"
                            "\"type\":\"math\","
                            "\"id\":\"math-1\","
                            "\"created\":\"2026-04-27T21:30:00Z\","
                            "\"updated\":\"2026-04-27T21:31:00Z\","
                            "\"content\":\"x^2 + y^2\","
                            "\"link_id\":\"jump-dest\","
                            "\"note_path\":\"lecture1/session1/note1.txt\","
                            "\"math_kind\":\"latex\","
                            "\"bbox\":[10,20,30,40],"
                            "\"pt\":12,"
                            "\"font\":\"Yu Gothic\","
                            "\"border\":\"rect\""
                        "}"
                    "]"
                "},"
                "{"
                    "\"page\":4,"
                    "\"items\":["
                        "{"
                            "\"type\":\"line\","
                            "\"id\":\"line-1\","
                            "\"created\":\"2026-04-27T21:30:00Z\","
                            "\"updated\":\"2026-04-27T21:31:00Z\","
                            "\"p1\":[1,2],"
                            "\"p2\":[3,4],"
                            "\"dash\":[1,2]"
                        "}"
                    "]"
                "}"
            "]"
            "}";

        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(ok, "valid clrop parses without DOM staging");
        if (ok) {
            Expect(doc.version == 1, "version is preserved");
            Expect(doc.pdfId.path == L"日本.pdf", "pdf_id.path unicode escape is decoded");
            Expect(doc.pdfId.size == 42, "pdf_id.size is preserved");
            Expect(doc.pdfId.pageCount == 2, "page_count falls back to valid page_sizes_pt entries");
            Expect(doc.pdfId.pageSizesPt.size() == 2, "page_sizes_pt entries are preserved");
            Expect(doc.pages.size() == 2, "valid page objects are preserved");
            if (doc.pages.size() >= 1) {
                Expect(doc.pages[0].page == 3, "first valid page index is preserved");
                Expect(doc.pages[0].items.size() == 3, "valid items on first page are preserved");
                if (doc.pages[0].items.size() >= 1) {
                    const auto& text = doc.pages[0].items[0];
                    Expect(text.kind == clrop::Item::Kind::Text, "text item kind is preserved");
                    Expect(text.id == L"text-1", "text item id is preserved");
                    Expect(text.content == L"hello", "text content is preserved");
                    Expect(text.lines.size() == 2, "string arrays are preserved");
                    Expect(text.bbox.has_value(), "bbox is parsed");
                }
                if (doc.pages[0].items.size() >= 2) {
                    const auto& marker = doc.pages[0].items[1];
                    Expect(marker.kind == clrop::Item::Kind::MarkerFree, "marker-free item kind is preserved");
                    Expect(marker.id == L"marker-1", "marker-free item id is preserved");
                    Expect(marker.path.size() == 4, "path segments are preserved");
                }
                if (doc.pages[0].items.size() >= 3) {
                    const auto& math = doc.pages[0].items[2];
                    Expect(math.kind == clrop::Item::Kind::Math, "math item kind is preserved");
                    Expect(math.id == L"math-1", "math item id is preserved");
                    Expect(math.created == L"2026-04-27T21:30:00Z", "math item created timestamp is preserved");
                    Expect(math.updated == L"2026-04-27T21:31:00Z", "math item updated timestamp is preserved");
                    Expect(math.linkId == L"jump-dest", "math item link_id is preserved");
                    Expect(math.notePath == L"lecture1/session1/note1.txt", "math item note_path is preserved");
                    Expect(math.mathKind == "latex", "math item math_kind is preserved");
                    Expect(math.border == "rect", "math item border is preserved");
                }
            }
            if (doc.pages.size() >= 2) {
                Expect(doc.pages[1].page == 4, "later valid pages are preserved");
                Expect(doc.pages[1].items.size() == 1, "later valid items are preserved");
                if (doc.pages[1].items.size() == 1) {
                    const auto& line = doc.pages[1].items[0];
                    Expect(line.kind == clrop::Item::Kind::Line, "known item type is parsed");
                    Expect(line.p1.has_value() && (*line.p1)[0] == 1 && (*line.p1)[1] == 2,
                           "line p1 is parsed");
                    Expect(line.dash.size() == 2, "numeric dash array is preserved");
                }
            }
        }

        CollectSink sink;
        clrop::PdfId sinkPdfId;
        err.clear();
        ok = clrop::ParseClropFromJsonToSink(json, &sinkPdfId, sink, err);
        Expect(ok, "valid clrop parses through sink path");
        if (ok) {
            Expect(sinkPdfId.path == doc.pdfId.path, "sink path preserves pdf_id.path");
            Expect(sinkPdfId.size == doc.pdfId.size, "sink path preserves pdf_id.size");
            Expect(sink.records.size() == 4, "sink path emits only valid items");
            if (sink.records.size() == 4) {
                Expect(sink.records[0].page == 3, "sink path preserves first item page");
                Expect(sink.records[0].item.kind == clrop::Item::Kind::Text,
                       "sink path preserves first item kind");
                Expect(sink.records[1].page == 3, "sink path preserves second item page");
                Expect(sink.records[1].item.kind == clrop::Item::Kind::MarkerFree,
                       "sink path preserves second item kind");
                Expect(sink.records[2].page == 3, "sink path preserves third item page");
                Expect(sink.records[2].item.kind == clrop::Item::Kind::Math,
                       "sink path preserves third item kind");
                Expect(sink.records[2].item.notePath == L"lecture1/session1/note1.txt",
                       "sink path preserves third item note_path");
                Expect(sink.records[3].page == 4, "sink path preserves later page number");
                Expect(sink.records[3].item.kind == clrop::Item::Kind::Line,
                       "sink path preserves later item kind");
            }
        }
    }

    {
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{"
                "\"path\":\"compact.pdf\","
                "\"size\":128,"
                "\"page_count\":5,"
                "\"page_size_rule\":{\"default_pt\":[595,842],\"exceptions\":[[2,612,792],[9,100,100]]},"
                "\"sha256\":\"rule\""
            "},"
            "\"pages\":[]"
            "}";

        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(ok, "compact page-size rule parses successfully");
        if (ok) {
            Expect(doc.pdfId.pageCount == 5, "compact rule preserves page_count");
            Expect(doc.pdfId.pageSizesPt.size() == 5, "compact rule expands to all page sizes");
            if (doc.pdfId.pageSizesPt.size() == 5) {
                Expect(doc.pdfId.pageSizesPt[0][0] == 595 && doc.pdfId.pageSizesPt[0][1] == 842,
                       "compact rule fills default size for first page");
                Expect(doc.pdfId.pageSizesPt[2][0] == 612 && doc.pdfId.pageSizesPt[2][1] == 792,
                       "compact rule applies exception by page index");
                Expect(doc.pdfId.pageSizesPt[4][0] == 595 && doc.pdfId.pageSizesPt[4][1] == 842,
                       "compact rule ignores out-of-range exceptions");
            }
        }
    }

    {
        clrop::Document doc;
        doc.version = 1;
        doc.pdfId.path = L"uniform.pdf";
        doc.pdfId.size = 77;
        doc.pdfId.pageCount = 10;
        doc.pdfId.pageSizesPt = {
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{595, 842},
            std::array<double, 2>{612, 792},
        };
        doc.pdfId.sha256 = "compact";
        clrop::Page page;
        page.page = 0;
        clrop::Item item;
        item.kind = clrop::Item::Kind::Text;
        item.id = L"roundtrip-id";
        item.created = L"2026-04-27T21:32:00Z";
        item.updated = L"2026-04-27T21:33:00Z";
        item.content = L"persist";
        item.linkId = L"dest-1";
        item.notePath = L"lecture1/session1/note1.txt";
        item.bbox = std::array<double, 4>{1.0, 2.0, 3.0, 4.0};
        page.items.push_back(item);
        doc.pages.push_back(std::move(page));

        std::string json;
        std::wstring err;
        bool ok = clrop::SerializeClrop(doc, json, err);
        Expect(ok, "serializer emits compact page-size rule");
        if (ok) {
            Expect(json.find("\"page_size_rule\"") != std::string::npos,
                   "serializer writes compact page-size rule field");
            Expect(json.find("\"page_sizes_pt\"") == std::string::npos,
                   "serializer omits raw page_sizes_pt when compact rule is smaller");

            clrop::Document reparsed;
            err.clear();
            ok = clrop::ParseClropFromJson(json, reparsed, err);
            Expect(ok, "serialized compact rule parses back");
            if (ok) {
                Expect(reparsed.pdfId.pageSizesPt.size() == 10,
                       "serialized compact rule restores all page sizes");
                if (reparsed.pdfId.pageSizesPt.size() == 10) {
                    Expect(reparsed.pdfId.pageSizesPt[9][0] == 612 &&
                               reparsed.pdfId.pageSizesPt[9][1] == 792,
                           "serialized compact rule preserves exception page size");
                }
                Expect(reparsed.pages.size() == 1 && reparsed.pages[0].items.size() == 1 &&
                           reparsed.pages[0].items[0].id == L"roundtrip-id",
                       "serializer preserves item id roundtrip");
                if (reparsed.pages.size() == 1 && reparsed.pages[0].items.size() == 1) {
                    const auto& reparsedItem = reparsed.pages[0].items[0];
                    Expect(reparsedItem.created == L"2026-04-27T21:32:00Z",
                           "serializer preserves item created timestamp");
                    Expect(reparsedItem.updated == L"2026-04-27T21:33:00Z",
                           "serializer preserves item updated timestamp");
                    Expect(reparsedItem.linkId == L"dest-1",
                           "serializer preserves item link_id");
                    Expect(reparsedItem.notePath == L"lecture1/session1/note1.txt",
                           "serializer preserves item note_path");
                }
            }
        }
    }

    {
        clrop::Document doc;
        doc.version = 1;
        doc.pdfId.path = L"mixed.pdf";
        doc.pdfId.size = 88;
        doc.pdfId.pageCount = 3;
        doc.pdfId.pageSizesPt = {
            std::array<double, 2>{500, 700},
            std::array<double, 2>{510, 710},
            std::array<double, 2>{520, 720},
        };
        doc.pdfId.sha256 = "raw";

        std::string json;
        std::wstring err;
        bool ok = clrop::SerializeClrop(doc, json, err);
        Expect(ok, "serializer still supports raw page-size list");
        if (ok) {
            Expect(json.find("\"page_sizes_pt\"") != std::string::npos,
                   "serializer keeps raw page_sizes_pt when compact rule is not beneficial");
        }
    }

    {
        const std::string json =
            "\xEF\xBB\xBF"
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"bom.pdf\",\"size\":7,\"sha256\":\"x\"},"
            "\"pages\":[{\"page\":0,\"items\":[{"
                "\"type\":\"text\","
                "\"id\":\"bom-text\","
                "\"created\":\"2026-04-27T21:30:00Z\","
                "\"updated\":\"2026-04-27T21:31:00Z\","
                "\"content\":\"bom\","
                "\"bbox\":[1,2,3,4]"
            "}]}]"
            "}";
        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(ok, "utf-8 bom is accepted at parse root");
        if (ok) {
            Expect(doc.pdfId.path == L"bom.pdf", "utf-8 bom preserves pdf_id.path");
            Expect(doc.pages.size() == 1 && doc.pages[0].items.size() == 1,
                   "utf-8 bom preserves parsed pages");
        }

        CollectSink sink;
        clrop::PdfId sinkPdfId;
        err.clear();
        ok = clrop::ParseClropFromJsonToSink(json, &sinkPdfId, sink, err);
        Expect(ok, "utf-8 bom is accepted through sink path");
        if (ok) {
            Expect(sinkPdfId.path == L"bom.pdf", "utf-8 bom preserves sink pdf_id.path");
            Expect(sink.records.size() == 1, "utf-8 bom preserves sink items");
        }
    }

    {
        const fs::path path = MakeTempClropPath(L"load_ok.clrop");
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"ok.pdf\",\"size\":11,\"sha256\":\"z\"},"
            "\"pages\":[{\"page\":2,\"items\":[{"
                "\"type\":\"text\","
                "\"id\":\"ok-text\","
                "\"created\":\"2026-04-27T21:30:00Z\","
                "\"updated\":\"2026-04-27T21:31:00Z\","
                "\"content\":\"ok\","
                "\"bbox\":[1,2,3,4]"
            "}]}]"
            "}";
        Expect(WriteRaw(path, json), "load-ok fixture is written");

        CollectSink sink;
        clrop::PdfId loadedId;
        std::wstring err;
        clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
        const bool ok = clrop::LoadClropFileToSink(path.wstring(), &loadedId, sink, err, &failureKind);
        Expect(ok, "clrop file loads successfully from disk");
        if (ok) {
            Expect(failureKind == clrop::LoadFileFailureKind::None,
                   "successful disk load keeps failure kind clear");
            Expect(loadedId.path == L"ok.pdf", "successful disk load preserves pdf_id.path");
            Expect(sink.records.size() == 1 && sink.records[0].page == 2,
                   "successful disk load preserves sink records");
        }

        std::error_code ec;
        fs::remove(path, ec);
    }

    {
        const fs::path path = MakeTempClropPath(L"parse_fail.clrop");
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"broken.pdf\"},"
            "\"pages\":[]"
            "}";
        Expect(WriteRaw(path, json), "parse-fail fixture is written");

        clrop::Document doc;
        std::wstring err;
        clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
        const bool ok = clrop::LoadClropFile(path.wstring(), doc, err, &failureKind);
        Expect(!ok, "parse-invalid clrop file reports parse failure");
        Expect(failureKind == clrop::LoadFileFailureKind::Parse,
               "parse-invalid clrop file is classified as parse failure");
        Expect(err == L"pdf_id.size missing",
               "parse-invalid clrop file keeps parser error text");

        std::error_code ec;
        fs::remove(path, ec);
    }

    {
        clrop::Document doc;
        doc.version = 1;
        doc.pdfId.path = L"roundtrip.pdf";
        doc.pdfId.size = 12;
        doc.pdfId.sha256 = "rt";

        clrop::Page page;
        page.page = 5;

        clrop::Item freehand;
        freehand.kind = clrop::Item::Kind::Freehand;
        freehand.id = L"freehand-1";
        freehand.created = L"2026-04-27T21:32:00Z";
        freehand.updated = L"2026-04-27T21:33:00Z";
        freehand.color = "#123456";
        freehand.alpha = 0.35;
        freehand.width = 2.5;
        freehand.path = {1.0, 2.0, 3.0, 4.0};
        page.items.push_back(freehand);

        clrop::Item shape;
        shape.kind = clrop::Item::Kind::Shape;
        shape.id = L"shape-1";
        shape.created = L"2026-04-27T21:32:00Z";
        shape.updated = L"2026-04-27T21:33:00Z";
        shape.color = "#abcdef";
        shape.alpha = 0.8;
        shape.width = 3.0;
        shape.shapeKind = "rotated_ellipse";
        shape.shapeDrawMode = "outline";
        shape.shapeRotation = 0.37;
        shape.bbox = std::array<double, 4>{10.0, 20.0, 30.0, 40.0};
        page.items.push_back(shape);

        doc.pages.push_back(page);

        std::string json;
        std::wstring err;
        bool ok = clrop::SerializeClrop(doc, json, err);
        Expect(ok, "shape draw mode and freehand alpha serialize");
        if (ok) {
            Expect(json.find("\"shape_draw_mode\":\"outline\"") != std::string::npos,
                   "serialized json includes shape_draw_mode");
            Expect(json.find("\"shape_rotation\":0.37") != std::string::npos,
                   "serialized json includes shape_rotation");
            clrop::Document parsed;
            err.clear();
            ok = clrop::ParseClropFromJson(json, parsed, err);
            Expect(ok, "serialized shape draw mode and freehand alpha parse back");
            if (ok) {
                Expect(parsed.pages.size() == 1 && parsed.pages[0].items.size() == 2,
                       "round-trip preserves item count");
                if (parsed.pages.size() == 1 && parsed.pages[0].items.size() == 2) {
                    const auto& parsedFreehand = parsed.pages[0].items[0];
                    const auto& parsedShape = parsed.pages[0].items[1];
                    Expect(parsedFreehand.kind == clrop::Item::Kind::Freehand,
                           "round-trip preserves freehand kind");
                    Expect(parsedFreehand.alpha == 0.35, "round-trip preserves freehand alpha");
                    Expect(parsedShape.kind == clrop::Item::Kind::Shape,
                           "round-trip preserves shape kind");
                    Expect(parsedShape.shapeDrawMode == "outline",
                           "round-trip preserves shape draw mode");
                    Expect(parsedShape.shapeRotation && Near(*parsedShape.shapeRotation, 0.37),
                           "round-trip preserves shape rotation");
                }
            }
        }
    }

    {
        clrop::Document doc;
        doc.version = 1;
        doc.pdfId.path = L"all-kinds.pdf";
        doc.pdfId.size = 12345;
        doc.pdfId.pageCount = 2;
        doc.pdfId.pageSizesPt = {
            std::array<double, 2>{595.25, 842.5},
            std::array<double, 2>{612.75, 792.125},
        };
        doc.pdfId.sha256 = "allkinds";

        clrop::Page page;
        page.page = 1;
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Text, L"kind-text", 0.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Math, L"kind-math", 1.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::MarkerText, L"kind-marker-text", 2.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::TextColor, L"kind-text-color", 3.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::MarkerFree, L"kind-marker-free", 4.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Line, L"kind-line", 5.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Arrow, L"kind-arrow", 6.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Wave, L"kind-wave", 7.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Freehand, L"kind-freehand", 8.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::LinkMarker, L"kind-link-marker", 9.0));
        page.items.push_back(MakeFullRoundTripItem(clrop::Item::Kind::Shape, L"kind-shape", 10.0));
        doc.pages.push_back(page);

        std::string json;
        std::wstring err;
        bool ok = clrop::SerializeClrop(doc, json, err);
        Expect(ok, "all clrop item kinds serialize");
        if (ok) {
            clrop::Document parsed;
            err.clear();
            ok = clrop::ParseClropFromJson(json, parsed, err);
            Expect(ok, "all clrop item kinds parse after serialization");
            if (ok) {
                Expect(parsed.pdfId.path == doc.pdfId.path &&
                           parsed.pdfId.size == doc.pdfId.size &&
                           parsed.pdfId.pageCount == doc.pdfId.pageCount,
                       "all-kinds round-trip preserves pdf_id identity fields");
                Expect(parsed.pdfId.pageSizesPt.size() == doc.pdfId.pageSizesPt.size(),
                       "all-kinds round-trip preserves page-size count");
                Expect(parsed.pages.size() == 1 && parsed.pages[0].items.size() == page.items.size(),
                       "all-kinds round-trip preserves every item");
                if (parsed.pages.size() == 1 && parsed.pages[0].items.size() == page.items.size()) {
                    bool allSame = true;
                    for (size_t i = 0; i < page.items.size(); ++i) {
                        if (!SameRoundTripItem(page.items[i], parsed.pages[0].items[i])) {
                            allSame = false;
                            break;
                        }
                    }
                    Expect(allSame, "all-kinds round-trip preserves item fields without loss");
                }
            }

            CollectSink sink;
            clrop::PdfId sinkPdfId;
            err.clear();
            ok = clrop::ParseClropFromJsonToSink(json, &sinkPdfId, sink, err);
            Expect(ok, "all clrop item kinds parse through sink path after serialization");
            if (ok) {
                Expect(sink.records.size() == page.items.size(),
                       "sink path emits every all-kinds item");
                bool allSinkSame = sink.records.size() == page.items.size();
                for (size_t i = 0; allSinkSame && i < page.items.size(); ++i) {
                    allSinkSame = sink.records[i].page == page.page &&
                                  SameRoundTripItem(page.items[i], sink.records[i].item);
                }
                Expect(allSinkSame, "sink path preserves all-kinds item fields without loss");
            }
        }
    }

    {
        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson("[]", doc, err);
        Expect(!ok, "non-object root is rejected");
        Expect(err == L"Root must be object", "non-object root keeps prior error text");
    }

    {
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"x\"},"
            "\"pages\":[]"
            "}";
        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(!ok, "missing pdf_id.size is rejected");
        Expect(err == L"pdf_id.size missing", "missing pdf_id.size keeps prior error text");
    }

    {
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"strict.pdf\",\"size\":1,\"sha256\":\"s\"},"
            "\"pages\":[{\"page\":0,\"items\":[{\"type\":\"text\",\"content\":\"old\",\"bbox\":[1,2,3,4]}]}]"
            "}";
        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(ok, "legacy item without id is skipped (but file load succeeds)");
        Expect(doc.pages.empty() || doc.pages[0].items.empty(), "legacy item without id is not loaded");
    }

    {
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"strict.pdf\",\"size\":1,\"sha256\":\"s\"},"
            "\"pages\":[{\"page\":0,\"items\":[{"
                "\"type\":\"unknown\","
                "\"id\":\"bad\","
                "\"created\":\"2026-04-27T21:30:00Z\","
                "\"updated\":\"2026-04-27T21:31:00Z\""
            "}]}]"
            "}";
        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(ok, "unknown clrop item type is skipped (but file load succeeds)");
        Expect(doc.pages.empty() || doc.pages[0].items.empty(), "unknown item type is not loaded");
    }

    {
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"strict.pdf\",\"size\":1,\"sha256\":\"s\"},"
            "\"pages\":[{\"page\":0,\"items\":[123]}]"
            "}";
        clrop::Document doc;
        std::wstring err;
        bool ok = clrop::ParseClropFromJson(json, doc, err);
        Expect(ok, "non-object clrop item is skipped (but file load succeeds)");
        Expect(doc.pages.empty() || doc.pages[0].items.empty(), "non-object item is not loaded");
    }

    {
        const fs::path path = MakeTempClropPath(L"locked.clrop");
        const std::string json =
            "{"
            "\"version\":1,"
            "\"pdf_id\":{\"path\":\"locked.pdf\",\"size\":9,\"sha256\":\"y\"},"
            "\"pages\":[]"
            "}";
        Expect(WriteRaw(path, json), "locked-file fixture is written");

        HANDLE lock = CreateFileW(path.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        Expect(lock != INVALID_HANDLE_VALUE, "locked-file fixture acquires exclusive handle");
        if (lock != INVALID_HANDLE_VALUE) {
            CollectSink sink;
            clrop::PdfId loadedId;
            std::wstring err;
            clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
            const bool ok = clrop::LoadClropFileToSink(path.wstring(), &loadedId, sink, err, &failureKind);
            Expect(!ok, "locked clrop file reports load failure");
            Expect(failureKind == clrop::LoadFileFailureKind::Read,
                   "locked clrop file is classified as read failure");
            Expect(err.find(L"failed to read clrop file") != std::wstring::npos,
                   "locked clrop file keeps read error prefix");
            Expect(err.find(path.wstring()) != std::wstring::npos,
                   "locked clrop file includes path in error text");
            CloseHandle(lock);
        }

        std::error_code ec;
        fs::remove(path, ec);
    }

    {
        const fs::path path = MakeTempClropPath(L"oversized_sparse.clrop");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        Expect(file != INVALID_HANDLE_VALUE, "oversized sparse clrop fixture is opened");
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            size.QuadPart = 256ll * 1024ll * 1024ll + 1ll;
            const bool sized = SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
            CloseHandle(file);
            Expect(sized, "oversized sparse clrop fixture is sized without allocating payload memory");

            CollectSink sink;
            std::wstring err;
            clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
            const bool ok = clrop::LoadClropFileToSink(path.wstring(), nullptr, sink, err, &failureKind);
            Expect(!ok, "oversized clrop file is rejected before allocation");
            Expect(failureKind == clrop::LoadFileFailureKind::Read,
                   "oversized clrop file is classified as a read failure");
        }
        std::error_code ec;
        fs::remove(path, ec);
    }

    {
        std::cout << "Testing: Malformed / Robustness cases...\n";
        
        auto test_fail = [](const std::string& label, const std::string& payload) {
            fs::path p = MakeTempClropPath(L"fail.json");
            WriteRaw(p, payload);
            CollectSink sink;
            std::wstring err;
            bool ok = clrop::LoadClropFileToSink(p.wstring(), nullptr, sink, err);
            Expect(!ok, (label + " (expected failure)").c_str());
            fs::remove(p);
        };

        test_fail("Empty string", "");
        test_fail("Truncated JSON", "{\"version\":1, \"pages\": [");
        test_fail("Non-object top-level", "[]");
        test_fail("Invalid version", "{\"version\":\"string-instead-of-int\"}");
        test_fail("Missing version", "{\"pages\":[]}");
        test_fail("NUL byte in JSON", std::string("{\"version\":1,\0\"pages\":[]}", 23));

        {
            clrop::Document doc;
            std::wstring err;
            const bool ok = clrop::ParseClropFromJson(
                "{\"version\":1,\"pdf_id\":{\"path\":\"\\uD800\",\"size\":1,\"sha256\":\"u\"},\"pages\":[]}",
                doc,
                err);
            Expect(ok, "isolated unicode surrogate escape is handled without parser crash");
            if (ok) {
                Expect(doc.pdfId.size == 1, "isolated unicode surrogate escape preserves surrounding fields");
            }
        }
        
        std::string deep = "{\"version\":1,\"pages\":";
        for (int i = 0; i < 1000; ++i) deep += "[";
        test_fail("Deep nesting", deep);

        std::string deepUnknown =
            "{\"version\":1,\"pdf_id\":{\"path\":\"deep.pdf\",\"size\":1,\"sha256\":\"d\"},"
            "\"pages\":[],\"unknown\":";
        for (int i = 0; i < 1000; ++i) deepUnknown += "[";
        deepUnknown += "0";
        for (int i = 0; i < 1000; ++i) deepUnknown += "]";
        deepUnknown += "}";
        test_fail("Unknown-field nesting depth limit", deepUnknown);

        {
            std::string hugeContent(1024 * 1024, 'x');
            const std::string json =
                "{\"version\":1,"
                "\"pdf_id\":{\"path\":\"huge.pdf\",\"size\":1,\"sha256\":\"h\"},"
                "\"pages\":[{\"page\":0,\"items\":[{"
                "\"type\":\"text\","
                "\"id\":\"huge-text\","
                "\"created\":\"2026-05-07T00:00:00Z\","
                "\"updated\":\"2026-05-07T00:00:00Z\","
                "\"content\":\"" + hugeContent + "\","
                "\"bbox\":[1,2,3,4]"
                "}]}]}";
            clrop::Document doc;
            std::wstring err;
            const bool ok = clrop::ParseClropFromJson(json, doc, err);
            Expect(ok, "1MB clrop text value parses without OOM/crash");
            if (ok) {
                Expect(doc.pages.size() == 1 && doc.pages[0].items.size() == 1,
                       "1MB clrop text item is preserved");
                if (doc.pages.size() == 1 && doc.pages[0].items.size() == 1) {
                    Expect(doc.pages[0].items[0].content.size() == hugeContent.size(),
                           "1MB clrop text length is preserved");
                }
            }
        }

        // Invalid item type
        {
            fs::path p = MakeTempClropPath(L"bad_type.json");
            WriteRaw(p, "{\"version\":1,\"pdf_id\":{\"size\":1},\"pages\":[{\"page\":0,\"items\":[{\"type\":\"ghost\",\"id\":\"g1\"}]}]}");
            CollectSink sink;
            std::wstring err;
            bool ok = clrop::LoadClropFileToSink(p.wstring(), nullptr, sink, err);
            if (!ok) {
                std::wcout << L"  Parser Error: " << err << std::endl;
            }
            Expect(ok, "Invalid item type (should skip or succeed but handle gracefully)");
            Expect(sink.records.empty(), "Should not load invalid item type");
            fs::remove(p);
        }

        // Missing required fields in item
        {
            fs::path p = MakeTempClropPath(L"missing_id.json");
            WriteRaw(p, "{\"version\":1,\"pdf_id\":{\"size\":1},\"pages\":[{\"page\":0,\"items\":[{\"type\":\"text\"}]}]}");
            CollectSink sink;
            std::wstring err;
            bool ok = clrop::LoadClropFileToSink(p.wstring(), nullptr, sink, err);
            if (!ok) {
                std::wcout << L"  Parser Error: " << err << std::endl;
            }
            Expect(ok, "Missing item ID (should skip gracefully)");
            Expect(sink.records.empty(), "Should not load item with missing ID");
            fs::remove(p);
        }
    }

    std::cout << "\nFinal Summary: " << (g_failed == 0 ? "ALL PASSED" : "SOME FAILED") << "\n";
    {
        std::error_code ec;
        fs::remove_all(TempClropRoot(), ec);
    }
    return g_failed == 0 ? 0 : 1;
}
