#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>

#include "core/annot_stage_replay.h"
#include "clrop/json.h"

#define PDF_NOTE_ANNOT_COMMAND_CODEC_ONLY
#include "pdf_view/annotation_store.cppinc"
#undef PDF_NOTE_ANNOT_COMMAND_CODEC_ONLY

std::wstring UTF8ToWide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                               static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUTF8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                               out.data(), n, nullptr, nullptr);
    return out;
}

namespace {
int checked = 0;
int failed = 0;
void Check(bool ok, const char* label) {
    ++checked;
    if (!ok) { ++failed; std::cerr << "FAIL: " << label << '\n'; }
}

Annotation Text(std::wstring id = L"text") {
    Annotation a;
    a.type = Annotation::Type::TextBox;
    a.id = std::move(id);
    a.pageIndex = 1;
    a.x1 = 443.998337641;
    a.y1 = 318.222620455;
    a.x2 = a.x1 + 51.125;
    a.y2 = a.y1 - 28.375;
    a.fontPt = 14.0023794622;
    a.text = L"test \u30c6\u30ad\u30b9\u30c8";
    a.textLines = {a.text};
    a.fontName = L"Segoe UI";
    return a;
}

AnnotCommand Update(const Annotation& a) {
    AnnotCommand cmd;
    cmd.kind = AnnotCommandKind::Update;
    cmd.beforeIndex = cmd.afterIndex = 0;
    cmd.before = cmd.after = a;
    cmd.after.text += L" edited";
    cmd.after.textLines = {cmd.after.text};
    cmd.after.fontPt += 0.0024;
    return cmd;
}

AnnotCommand RoundTrip(AnnotCommand cmd, bool legacy = false) {
    if (legacy) cmd.numericPrecision = AnnotNumericPrecision::Legacy6;
    std::string json;
    Check(SerializeAnnotCommandsJson({cmd}, &json), "serialize command");
    if (legacy) {
        const std::string field = ",\"numeric_precision\":6";
        const auto pos = json.find(field);
        Check(pos != std::string::npos, "legacy precision marker exists");
        if (pos != std::string::npos) json.erase(pos, field.size());
    } else {
        Check(json.find("\"numeric_precision\":17") != std::string::npos, "new precision marker");
    }
    std::vector<AnnotCommand> parsed;
    Check(DeserializeAnnotCommandsJson(json, &parsed) && parsed.size() == 1, "deserialize command");
    return parsed.empty() ? AnnotCommand{} : parsed.front();
}

void ExpectRejected(std::vector<Annotation> annots, const AnnotCommand& cmd,
                    AnnotNumericPrecision precision, const char* label) {
    const auto before = annots;
    annot_stage_replay::State state(annots.size(), precision);
    Check(!state.Apply(&annots, cmd), label);
    Check(AnnotationListsMatchExactly(annots, before) &&
          (annots.empty() || annots.front().textLines == before.front().textLines), "rejection keeps annotations");
    if (cmd.kind == AnnotCommandKind::Update && annots.size() == 1) {
        auto valid = Update(annots.front());
        Check(state.Apply(&annots, valid), "rejected command keeps precision provenance usable");
    }
}

void RunFollowupCases() {
    Check(annot_stage_replay::DecimalRoundingBound(1000.0, 12) >= 5e-9L,
          "decimal bound uses stored exponent at a power of ten");
    Check(annot_stage_replay::DecimalRoundingBound(1e-307, 12) >= 5e-319L,
          "decimal bound preserves a subnormal rounding quantum");
    // The journal codec must preserve the same dash data as the checkpoint,
    // even where rendering chooses a normalized display pattern.
    for (const auto& pattern : {std::vector<double>{0.125, 300.0},
                                std::vector<double>{2.0},
                                std::vector<double>{0.0, -1.0, 2.0}}) {
        auto a = Text(); a.type = Annotation::Type::Line; a.dash = pattern;
        auto decoded = RoundTrip(Update(a));
        Check(decoded.before.dash == pattern && decoded.after.dash == pattern,
              "dash entries and length survive journal round trip");
        std::vector<Annotation> annots{a};
        annot_stage_replay::State state(1, AnnotNumericPrecision::RoundTrip17);
        Check(state.Apply(&annots, decoded) && annots.front().dash == pattern,
              "unmodified dash precondition replays without normalization");
        annots = {a};
        annot_stage_replay::State old(1, AnnotNumericPrecision::Legacy12);
        auto oldCommand = RoundTrip(Update(a), true);
        Check(old.Apply(&annots, oldCommand) && annots.front().dash == pattern,
              "legacy journal also preserves dash entries and count");
        AnnotCommand add; add.after = a; add.afterIndex = 0;
        add = RoundTrip(add);
        annots.clear();
        annot_stage_replay::State empty(0, AnnotNumericPrecision::RoundTrip17);
        Check(empty.Apply(&annots, add) && annots.front().dash == pattern,
              "adding a dashed annotation preserves saved data");
    }

    std::vector<AnnotCommand> parsed;
    for (const char* bad : {"\"text\":42", "\"font\":false", "\"id\":null",
                           "\"color\":\"invalid\"", "\"color\":-1", "\"color\":1.5",
                           "\"color\":4294967296", "\"link_id\":7", "\"note_path\":false",
                           "\"shape_kind\":[]", "\"shape_draw_mode\":null", "\"arrow_head\":1",
                           "\"math_kind\":false", "\"math_kind\":\"unknown\"",
                           "\"writing_mode\":null", "\"writing_mode\":\"unknown\"",
                           "\"background_assist_mode\":7", "\"background_assist_mode\":\"unknown\"",
                           "\"readable_bg\":1", "\"readable_bg_configured\":null",
                           "\"readable_bg_inverted\":[]", "\"text\":\"a\",\"text\":\"b\""}) {
        const std::string json = std::string("[{\"kind\":\"add\",\"afterIndex\":0,\"after\":{") +
            "\"type\":\"text_box\",\"page\":1," + bad + "}}]";
        Check(!DeserializeAnnotCommandsJson(json, &parsed) && parsed.empty(),
              "malformed content cannot become defaults or publish commands");
    }
    for (const char* bad : {"\"after\":42", "\"before\":false", "\"snapshot\":null",
                           "\"after_snapshot\":{}", "\"afterIndex\":0,\"afterIndex\":1"}) {
        const std::string json = std::string("[{\"kind\":\"add\",") + bad + "}]";
        Check(!DeserializeAnnotCommandsJson(json, &parsed) && parsed.empty(),
              "malformed or duplicate command fields are rejected");
    }
    Check(DeserializeAnnotCommandsJson(
              R"([{"kind":"add","afterIndex":0,"after":{"type":"text_box","id":"x","page":1}}])",
              &parsed) && parsed.size() == 1, "absent legacy content fields keep their defaults");
    auto color = Update(Text()); color.before.color = color.after.color = 0xffffffffu;
    Check(RoundTrip(color).before.color == 0xffffffffu, "full unsigned COLORREF range round trips");

    // Reproduce CLROP's independently rounded top and height. A journal's
    // endpoint may retain many more digits after cancellation near zero.
    for (auto type : {Annotation::Type::TextBox, Annotation::Type::MathBox, Annotation::Type::Shape}) {
        auto a = Text(); a.type = type;
        a.y1 = 1000.12345678912; a.y2 = 0.001234567890123;
        auto checkpoint = a;
        checkpoint.y1 = std::stod(annot_stage_replay::Rounded(a.y1, 12));
        const double height = std::stod(annot_stage_replay::Rounded(a.y1 - a.y2, 12));
        checkpoint.y2 = checkpoint.y1 - height;
        for (bool legacy : {false, true}) {
            std::vector<Annotation> annots{checkpoint};
            annot_stage_replay::State state(1, AnnotNumericPrecision::Legacy12);
            auto cmd = RoundTrip(Update(a), legacy);
            Check(state.Apply(&annots, cmd), "legacy bbox cancellation permits valid journal");
            auto bad = cmd; bad.before.y2 += 0.000001;
            ExpectRejected({checkpoint}, bad, AnnotNumericPrecision::Legacy12,
                           "bbox endpoint change outside operand rounding is rejected");
        }
        auto badOrigin = Update(a); badOrigin.before.y1 += 0.001;
        ExpectRejected({checkpoint}, badOrigin, AnnotNumericPrecision::Legacy12,
                       "bbox allowance cannot relax origin matching");
        ExpectRejected({checkpoint}, Update(a), AnnotNumericPrecision::RoundTrip17,
                       "new checkpoint cannot inherit old bbox rounding");
        auto line = checkpoint; line.type = Annotation::Type::Line;
        auto wanted = a; wanted.type = Annotation::Type::Line;
        ExpectRejected({line}, Update(wanted), AnnotNumericPrecision::Legacy12,
                       "direct line coordinates cannot use bbox rounding");
    }
    auto zero = Text(); zero.y1 = 1000.12345678912; zero.y2 = 1e-10;
    auto checkpointZero = zero;
    checkpointZero.y1 = std::stod(annot_stage_replay::Rounded(zero.y1, 12));
    checkpointZero.y2 = checkpointZero.y1 - std::stod(annot_stage_replay::Rounded(zero.y1 - zero.y2, 12));
    std::vector<Annotation> annots{checkpointZero};
    annot_stage_replay::State zeroState(1, AnnotNumericPrecision::Legacy12);
    Check(checkpointZero.y2 == 0 && zeroState.Apply(&annots, RoundTrip(Update(zero))),
          "legacy bbox cancellation to zero is bounded and recoverable");
    auto badZero = Update(zero); badZero.before.y2 = 1e-6;
    ExpectRejected({checkpointZero}, badZero, AnnotNumericPrecision::Legacy12,
                   "zero bbox cannot match an endpoint outside its rounding bound");
    annots = {zero};
    annot_stage_replay::State journalCoordinates(1, AnnotNumericPrecision::Legacy12);
    auto cmd = RoundTrip(Update(zero), true);
    Check(journalCoordinates.Apply(&annots, cmd), "legacy update advances bbox precision provenance");
    auto forged = Update(annots.front()); forged.before.y2 += 1e-9;
    Check(!journalCoordinates.Apply(&annots, forged), "journal coordinates lose checkpoint bbox allowance");
}

void Run() {
    const auto original = Text();
    auto legacy = RoundTrip(Update(original), true);
    Check(legacy.numericPrecision == AnnotNumericPrecision::Legacy6, "absent precision means legacy six");
    std::vector<Annotation> annots{original};
    Check(!ApplyAnnotCommandToList(&annots, legacy), "editing precondition stays exact");
    annot_stage_replay::State oldState(1, AnnotNumericPrecision::Legacy12);
    Check(oldState.Apply(&annots, legacy), "reported checkpoint/journal precision mismatch replays");
    Check(annots[0].text == legacy.after.text, "legacy edit restored");
    auto modern = RoundTrip(Update(original));
    Check(AnnotationEquals(modern.before, original), "17-digit journal preserves every double");
    annots = {original};
    annot_stage_replay::State newState(1, AnnotNumericPrecision::RoundTrip17);
    Check(newState.Apply(&annots, modern), "modern update applies");

    auto changed = original;
    changed.x1 += 0.01;
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "legacy real coordinate change rejected");
    changed = original; changed.x1 += 0.00000001;
    ExpectRejected({changed}, modern, AnnotNumericPrecision::RoundTrip17, "modern sub-six-digit edit rejected");
    changed = original; changed.x1 += 0.000000001;
    ExpectRejected({changed}, modern, AnnotNumericPrecision::Legacy12, "old checkpoint twelve-digit mismatch rejected");
    changed = original; changed.text += L" other";
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different text rejected");
    changed = original; changed.textLines.push_back(L"other");
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different lines rejected");
    changed = original; changed.writingMode = TextWritingMode::VerticalRl;
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different writing mode rejected");
    changed = original; changed.id = L"other";
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different id rejected");
    changed = original; changed.pageIndex++;
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different page rejected");
    changed = original; changed.color++;
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different color rejected");
    changed = original; changed.fontName = L"other";
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different font rejected");
    changed = original; changed.linkId = L"other";
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different link rejected");
    changed = original; changed.backgroundAssistMode = TextBackgroundAssistMode::Auto;
    ExpectRejected({changed}, legacy, AnnotNumericPrecision::Legacy12, "different assist mode rejected");
    auto wrongIndex = modern; wrongIndex.beforeIndex = 1;
    ExpectRejected({original}, wrongIndex, AnnotNumericPrecision::RoundTrip17, "wrong index rejected");

    auto vectorAnn = original;
    vectorAnn.path = {{0.123456789012345, 500.123456789012345}};
    vectorAnn.dash = {1.12345678901234, 2.12345678901234};
    vectorAnn.quads = {1.12345678901234, 2.12345678901234, 3, 4, 5, 6, 7, 8};
    auto vectorCmd = RoundTrip(Update(vectorAnn));
    Check(AnnotationEquals(vectorCmd.before, vectorAnn), "path dash and quads round trip");
    auto vectorLegacy = RoundTrip(Update(vectorAnn), true);
    annots = {vectorAnn};
    annot_stage_replay::State vectors(1, AnnotNumericPrecision::Legacy12);
    Check(vectors.Apply(&annots, vectorLegacy), "legacy vector rounding replays");
    auto vectorChanged = vectorAnn; vectorChanged.quads[0] += 0.01;
    ExpectRejected({vectorChanged}, vectorLegacy, AnnotNumericPrecision::Legacy12, "changed quads rejected");
    vectorChanged = vectorAnn; vectorChanged.path.push_back({1, 2});
    ExpectRejected({vectorChanged}, vectorLegacy, AnnotNumericPrecision::Legacy12, "changed vector length rejected");

    auto nonfinite = modern; nonfinite.after.fontPt = std::numeric_limits<double>::infinity();
    ExpectRejected({original}, nonfinite, AnnotNumericPrecision::RoundTrip17, "infinite output rejected");
    std::string untouched = "unchanged";
    Check(!SerializeAnnotCommandsJson({nonfinite}, &untouched) && untouched == "unchanged", "nonfinite serialization fails without output");
    nonfinite.after.fontPt = std::numeric_limits<double>::quiet_NaN();
    ExpectRejected({original}, nonfinite, AnnotNumericPrecision::RoundTrip17, "NaN rejected");

    Check(annot_stage_replay::NumberMatches(1.0, std::nextafter(1.0, 2.0), 17), "one ULP noise accepted");
    double distant = 1.0; for (int i = 0; i < 9; ++i) distant = std::nextafter(distant, 2.0);
    Check(!annot_stage_replay::NumberMatches(1.0, distant, 17), "nine ULP change rejected");
    Check(!annot_stage_replay::NumberMatches(0, std::numeric_limits<double>::denorm_min(), 17), "zero nonzero mismatch rejected");
    Check(!annot_stage_replay::NumberMatches(-1, 1, 6), "sign mismatch rejected");
    Check(!annot_stage_replay::NumberMatches(-0.0001, 0.0001, 17, 1e15), "bbox bound cannot cross zero");
    Check(!annot_stage_replay::NumberMatches(1000000, 1000010, 6), "large magnitude decimal bin mismatch rejected");
    const double endpoint = 0.001234567890123;
    const double rebuilt = 1000.0 - (1000.0 - endpoint);
    Check(annot_stage_replay::NumberMatches(endpoint, rebuilt, 17, 1000.0), "bbox cancellation bounded by axis operands");

    // A new journal entry on an old checkpoint must not inherit old precision.
    annots = {original};
    annot_stage_replay::State mixed(1, AnnotNumericPrecision::Legacy12);
    AnnotCommand add;
    add.afterIndex = 1; add.after = Text(L"added");
    add.after.x1 = 1000.123456789123;
    Check(mixed.Apply(&annots, add), "append to legacy checkpoint");
    auto forged = Update(annots[1]); forged.beforeIndex = forged.afterIndex = 1;
    forged.before.x1 += 0.000000001;
    Check(!mixed.Apply(&annots, forged) && annots.size() == 2, "new annotation cannot use old twelve-digit tolerance");
    auto duplicate = add; duplicate.afterIndex = 2;
    Check(!mixed.Apply(&annots, duplicate) && annots.size() == 2, "duplicate id rejected");

    AnnotCommand reorder;
    reorder.kind = AnnotCommandKind::Reorder;
    reorder.snapshot = annots;
    reorder.afterSnapshot = {annots[1], annots[0]};
    Check(mixed.Apply(&annots, reorder) && annots[0].id == L"added", "reorder preserves values and provenance");
    forged = Update(annots[0]); forged.before.x1 += 0.000000001;
    Check(!mixed.Apply(&annots, forged), "reorder retains new precision");
    reorder.snapshot = annots; reorder.afterSnapshot = {annots[1], annots[0]};
    reorder.afterSnapshot[0].text += L" hidden edit";
    Check(!mixed.Apply(&annots, reorder), "reorder cannot carry a hidden edit");
    reorder.afterSnapshot = {annots[0], annots[0]};
    Check(!mixed.Apply(&annots, reorder), "reorder duplicate rejected");

    AnnotCommand remove;
    remove.kind = AnnotCommandKind::Remove; remove.beforeIndex = 0; remove.before = annots[0];
    Check(mixed.Apply(&annots, remove) && annots.size() == 1, "remove advances provenance");
    AnnotCommand clear;
    clear.kind = AnnotCommandKind::ClearAll; clear.snapshot = annots;
    auto badClear = clear; badClear.snapshot[0].text += L" mismatch";
    Check(!mixed.Apply(&annots, badClear) && annots.size() == 1, "clear mismatched snapshot rejected");
    Check(mixed.Apply(&annots, clear) && annots.empty(), "clear exact snapshot accepted");

    clrop::Document doc;
    doc.version = 1;
    clrop::Page page; page.page = 1;
    clrop::Item item; item.kind = clrop::Item::Kind::Text; item.id = original.id;
    item.created = item.updated = L"2026-10-08T00:00:00Z";
    item.content = original.text; item.pt = original.fontPt;
    item.bbox = std::array<double, 4>{original.x1, original.y1, original.x2-original.x1, original.y1-original.y2};
    page.items.push_back(item); doc.pages.push_back(page);
    std::wstring error; std::string json;
    Check(clrop::SerializeClrop(doc, json, error), "CLROP serialization");
    clrop::Document loaded;
    Check(clrop::ParseClropFromJson(json, loaded, error), "CLROP parse");
    Check(loaded.pages.size() == 1 && loaded.pages[0].items.size() == 1 &&
          loaded.pages[0].items[0].pt == original.fontPt &&
          loaded.pages[0].items[0].bbox == item.bbox, "CLROP preserves all 17 digits");

    std::vector<AnnotCommand> parsed;
    Check(!DeserializeAnnotCommandsJson("[{\"kind\":\"add\",\"numeric_precision\":18}]", &parsed) && parsed.empty(), "unknown precision rejected");
    Check(!DeserializeAnnotCommandsJson("[{\"kind\":\"add\",\"numeric_precision\":17,\"numeric_precision\":6}]", &parsed) && parsed.empty(), "duplicate precision rejected");
    Check(!DeserializeAnnotCommandsJson("[{\"kind\":\"update\",\"before\":{\"type\":\"bad\"}}]", &parsed) && parsed.empty(), "malformed annotation rejected");
    std::string valid; Check(SerializeAnnotCommandsJson({modern}, &valid), "valid JSON for mixed parse");
    valid.pop_back(); valid += ",{\"kind\":\"unknown\"}]";
    Check(!DeserializeAnnotCommandsJson(valid, &parsed) && parsed.empty(), "parse failure publishes no partial commands");

    for (const char* malformed : {"\"x1\":\"bad\"", "\"alpha\":false", "\"fontPt\":null",
                                  "\"path\":[[1,\"bad\"]]", "\"path\":[[1,2,3]]",
                                  "\"quads\":[1,false]", "\"dash\":false", "\"lines\":[1]"}) {
        const std::string broken = std::string("[{\"kind\":\"update\",\"before\":{") +
            "\"type\":\"text_box\",\"id\":\"text\",\"page\":1," + malformed + "}}]";
        Check(!DeserializeAnnotCommandsJson(broken, &parsed) && parsed.empty(), "malformed present field cannot default or drop data");
    }
    Check(!DeserializeAnnotCommandsJson("[{\"kind\":\"add\",\"afterIndex\":1e300}]", &parsed), "overflowing index rejected before cast");
    Check(!DeserializeAnnotCommandsJson("[{\"kind\":\"add\",\"afterIndex\":0.5}]", &parsed), "fractional index rejected without rounding");

    auto full = original;
    full.x1 = 443.998337641231;
    full.x2 = full.x1 + 51.125;
    full.fontPt = 14.00237946223456;
    auto twelve = full;
    twelve.x1 = 443.998337641; twelve.x2 = twelve.x1 + 51.125; twelve.fontPt = 14.0023794622;
    annots = {twelve};
    annot_stage_replay::State oldCheckpointModernJournal(1, AnnotNumericPrecision::Legacy12);
    Check(oldCheckpointModernJournal.Apply(&annots, RoundTrip(Update(full))), "old checkpoint with precise new journal before state");

    for (double value : {std::numeric_limits<double>::denorm_min(),
                         std::numeric_limits<double>::min(),
                         std::numeric_limits<double>::max(), -0.12345678901234567}) {
        auto edge = Update(original); edge.before.fontPt = edge.after.fontPt = value;
        const auto decoded = RoundTrip(edge);
        Check(decoded.before.fontPt == value, "edge double round trip in journal");
        doc.pages[0].items[0].pt = value;
        Check(clrop::SerializeClrop(doc, json, error) && clrop::ParseClropFromJson(json, loaded, error) &&
              loaded.pages.size() == 1 && loaded.pages[0].items.size() == 1 &&
              loaded.pages[0].items[0].pt == value, "edge double round trip in CLROP");
    }

    annots = {original};
    annot_stage_replay::State unsupported(1, AnnotNumericPrecision::Unsupported);
    Check(!unsupported.Apply(&annots, modern) && AnnotationEquals(annots[0], original), "unsupported checkpoint rejected unchanged");
    annots.clear();
    annot_stage_replay::State emptyUnsupported(0, AnnotNumericPrecision::Unsupported);
    auto unknownAdd = add; unknownAdd.afterIndex = 0;
    Check(!emptyUnsupported.Apply(&annots, unknownAdd) && annots.empty(), "empty unsupported checkpoint cannot accept additions");

    struct Comma : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    const auto prior = std::locale();
    std::locale::global(std::locale(prior, new Comma));
    std::string localized;
    Check(SerializeAnnotCommandsJson({modern}, &localized), "command serialization with comma locale");
    Check(DeserializeAnnotCommandsJson(localized, &parsed), "command JSON stays locale independent");
    Check(clrop::SerializeClrop(doc, localized, error) && clrop::ParseClropFromJson(localized, loaded, error), "CLROP stays locale independent");
    std::locale::global(prior);
    RunFollowupCases();
}
} // namespace

int main() {
    Run();
    std::cout << "Annotation stage replay: " << checked << " checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}
