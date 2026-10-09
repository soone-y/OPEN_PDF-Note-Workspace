#pragma once

#include "core/annot_command_apply.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace annot_stage_replay {

inline constexpr bool IsCheckpointPrecision(AnnotNumericPrecision precision) {
    return precision == AnnotNumericPrecision::Legacy12 ||
           precision == AnnotNumericPrecision::RoundTrip17;
}

inline bool IsFinite(const Annotation& a) {
    for (double v : {a.x1, a.y1, a.x2, a.y2, a.width, a.alpha, a.fontPt, a.shapeRotation}) {
        if (!std::isfinite(v)) return false;
    }
    for (const auto& p : a.path) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    }
    for (double v : a.dash) if (!std::isfinite(v)) return false;
    for (double v : a.quads) if (!std::isfinite(v)) return false;
    return true;
}

inline std::string Rounded(double value, int digits) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(digits) << std::defaultfloat << value;
    return stream.str();
}

inline bool NumberMatches(double current, double expected, int digits,
                          double reconstructionScale = 0.0) {
    if (!std::isfinite(current) || !std::isfinite(expected)) return false;
    if (current == expected) return true;
    // Zero and a nonzero value never share a decimal rounding bin.
    if (current == 0.0 || expected == 0.0 ||
        std::signbit(current) != std::signbit(expected)) return false;
    if (digits == 6 || digits == 12) {
        return Rounded(current, digits) == Rounded(expected, digits);
    }
    if (digits != 17) return false;
    // Only representational noise, not a general relative/absolute epsilon.
    double adjacent = current;
    for (int i = 0; i < 8; ++i) {
        adjacent = std::nextafter(adjacent, expected);
        if (adjacent == expected) return true;
    }
    // CLROP stores bbox origin + extent. Rebuilding x2/y2 can lose low bits
    // through subtraction; bound that error by the actual axis operands.
    return reconstructionScale > 0.0 && std::isfinite(reconstructionScale) &&
           std::abs(current - expected) <=
               4.0 * std::numeric_limits<double>::epsilon() * reconstructionScale;
}

inline bool UsesCheckpointBBox(Annotation::Type type) {
    return type == Annotation::Type::TextBox || type == Annotation::Type::MathBox ||
           type == Annotation::Type::Shape;
}

inline long double DecimalRoundingBound(double value, int digits) {
    if (value == 0.0) return 0.0L;
    // Derive the decimal exponent from the stored representation, avoiding
    // log10 rounding on either side of a power-of-ten boundary.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::scientific << std::setprecision(digits - 1) << value;
    const std::string text = stream.str();
    const size_t e = text.find('e');
    if (e == std::string::npos) return std::numeric_limits<long double>::infinity();
    const char* begin = text.data() + e + 1;
    if (*begin == '+') ++begin;
    int exponent = 0;
    const auto parsed = std::from_chars(begin, text.data() + text.size(), exponent);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::numeric_limits<long double>::infinity();
    // One binary ULP also covers decimal-to-double conversion.
    const long double decimal = 0.5L * std::pow(10.0L, exponent - digits + 1);
    const long double ulp = std::abs(static_cast<long double>(value) -
                                    std::nextafter(value, 0.0));
    return decimal + ulp;
}

// Only a 12-digit checkpoint bbox has two independently rounded operands.
// Anchor at its stored origin (already matched separately), not at the more
// coarsely rounded journal origin. Direct journal coordinates never get this
// allowance. Zero/sign changes can result from this subtraction, but only
// inside the explicitly derived operand-rounding bound.
inline bool LegacyBBoxEndpointMatches(double current, double expected,
                                      double storedOrigin, bool subtractExtent,
                                      AnnotNumericPrecision expectedPrecision) {
    if (!std::isfinite(current) || !std::isfinite(expected) ||
        !std::isfinite(storedOrigin)) return false;
    const double extent = subtractExtent ? storedOrigin - current : current - storedOrigin;
    if (!std::isfinite(extent) || extent < 0.0) return false;
    const long double scale = std::max(std::abs(static_cast<long double>(storedOrigin)),
                                       std::abs(static_cast<long double>(extent)));
    const long double bound = DecimalRoundingBound(storedOrigin, 12) +
                              DecimalRoundingBound(extent, 12) +
                              (expectedPrecision == AnnotNumericPrecision::Legacy6
                                   ? DecimalRoundingBound(expected, 6) : 0.0L) +
                              4.0L * std::numeric_limits<double>::epsilon() * scale;
    return std::isfinite(bound) &&
           std::abs(static_cast<long double>(current) - expected) <= bound;
}

inline bool AnnotationMatches(const Annotation& current, const Annotation& expected,
                              AnnotNumericPrecision currentPrecision,
                              AnnotNumericPrecision expectedPrecision) {
    if (current.id.empty() || current.id != expected.id ||
        current.textLines != expected.textLines || current.writingMode != expected.writingMode ||
        !IsFinite(current) || !IsFinite(expected)) return false;
    const int digits = std::min(static_cast<int>(currentPrecision),
                                static_cast<int>(expectedPrecision));
    Annotation aligned = current;
    const double xScale = std::max({std::abs(current.x1), std::abs(current.x2),
                                    std::abs(expected.x1), std::abs(expected.x2)});
    const double yScale = std::max({std::abs(current.y1), std::abs(current.y2),
                                    std::abs(expected.y1), std::abs(expected.y2)});
    auto align = [digits](double& actual, double wanted, double scale = 0.0) {
        if (!NumberMatches(actual, wanted, digits, scale)) return false;
        actual = wanted;
        return true;
    };
    const bool bbox = UsesCheckpointBBox(current.type) && current.type == expected.type;
    auto alignEndpoint = [&](double& actual, double wanted, double origin,
                             bool subtractExtent, double scale) {
        if (!NumberMatches(actual, wanted, digits, bbox ? scale : 0.0) &&
            !(bbox && currentPrecision == AnnotNumericPrecision::Legacy12 &&
              LegacyBBoxEndpointMatches(actual, wanted, origin, subtractExtent, expectedPrecision))) return false;
        actual = wanted;
        return true;
    };
    if (!align(aligned.x1, expected.x1) || !align(aligned.y1, expected.y1) ||
        !alignEndpoint(aligned.x2, expected.x2, current.x1, false, xScale) ||
        !alignEndpoint(aligned.y2, expected.y2, current.y1, true, yScale) ||
        !align(aligned.width, expected.width) || !align(aligned.alpha, expected.alpha) ||
        !align(aligned.fontPt, expected.fontPt) ||
        !align(aligned.shapeRotation, expected.shapeRotation)) return false;
    if (aligned.path.size() != expected.path.size() || aligned.dash.size() != expected.dash.size() ||
        aligned.quads.size() != expected.quads.size()) return false;
    for (size_t i = 0; i < aligned.path.size(); ++i) {
        if (!align(aligned.path[i].x, expected.path[i].x) ||
            !align(aligned.path[i].y, expected.path[i].y)) return false;
    }
    for (size_t i = 0; i < aligned.dash.size(); ++i) {
        if (!align(aligned.dash[i], expected.dash[i])) return false;
    }
    for (size_t i = 0; i < aligned.quads.size(); ++i) {
        if (!align(aligned.quads[i], expected.quads[i])) return false;
    }
    return AnnotationEquals(aligned, expected);
}

inline bool ValidList(const std::vector<Annotation>& annots) {
    std::unordered_set<std::wstring> ids;
    for (const auto& a : annots) {
        if (a.id.empty() || !ids.insert(a.id).second || !IsFinite(a)) return false;
    }
    return true;
}

// Owns precision provenance for one in-memory replay, never files. On rejection
// both annotations and precision state remain unchanged. Editing/Undo stay exact.
class State {
public:
    State(size_t count, AnnotNumericPrecision checkpointPrecision)
        : precisions_(count, checkpointPrecision), supported_(IsCheckpointPrecision(checkpointPrecision)) {}

    [[nodiscard]] bool Apply(std::vector<Annotation>* annots, const AnnotCommand& cmd) {
        if (!supported_ || !annots || precisions_.size() != annots->size() ||
            (cmd.numericPrecision != AnnotNumericPrecision::Legacy6 &&
             cmd.numericPrecision != AnnotNumericPrecision::RoundTrip17)) return false;
        try {
            for (const auto p : precisions_) {
                if (p != AnnotNumericPrecision::Legacy6 && p != AnnotNumericPrecision::Legacy12 &&
                    p != AnnotNumericPrecision::RoundTrip17) return false;
            }
            if (!ValidList(*annots)) return false;
            const auto& next = *annots;
            auto precision = precisions_;
            auto adjusted = cmd;
            auto matchAt = [&](int index, const Annotation& expected) {
                return index >= 0 && index < static_cast<int>(next.size()) &&
                       AnnotationMatches(next[static_cast<size_t>(index)], expected,
                                         precision[static_cast<size_t>(index)], cmd.numericPrecision);
            };
            switch (cmd.kind) {
            case AnnotCommandKind::Add:
                if (!IsFinite(cmd.after)) return false;
                if (cmd.afterIndex < 0 || cmd.afterIndex > static_cast<int>(next.size())) return false;
                precision.insert(precision.begin() + cmd.afterIndex, cmd.numericPrecision);
                break;
            case AnnotCommandKind::Remove:
            case AnnotCommandKind::Update:
                if (!matchAt(cmd.beforeIndex, cmd.before)) return false;
                adjusted.before = next[static_cast<size_t>(cmd.beforeIndex)];
                precision.erase(precision.begin() + cmd.beforeIndex);
                if (cmd.kind == AnnotCommandKind::Update) {
                    if (!IsFinite(cmd.after)) return false;
                    if (cmd.afterIndex < 0 || cmd.afterIndex > static_cast<int>(precision.size())) return false;
                    precision.insert(precision.begin() + cmd.afterIndex, cmd.numericPrecision);
                }
                break;
            case AnnotCommandKind::Reorder:
            case AnnotCommandKind::ClearAll:
                if (cmd.snapshot.size() != next.size()) return false;
                for (size_t i = 0; i < next.size(); ++i) {
                    if (!matchAt(static_cast<int>(i), cmd.snapshot[i])) return false;
                }
                adjusted.snapshot = next;
                if (cmd.kind == AnnotCommandKind::Reorder) {
                    // Reorder must only permute annotations, not edit their contents.
                    if (cmd.afterSnapshot.size() != cmd.snapshot.size()) return false;
                    std::unordered_set<std::wstring> seen;
                    std::vector<AnnotNumericPrecision> reordered;
                    for (const auto& a : cmd.afterSnapshot) {
                        const int index = FindAnnotIndexByIdInList(cmd.snapshot, a.id);
                        if (index < 0 || !seen.insert(a.id).second ||
                            !AnnotationMatches(cmd.snapshot[static_cast<size_t>(index)], a,
                                               cmd.numericPrecision, cmd.numericPrecision)) return false;
                        // Preserve the original values and their precision on permutation.
                        reordered.push_back(precision[static_cast<size_t>(index)]);
                    }
                    adjusted.afterSnapshot.clear();
                    for (const auto& a : cmd.afterSnapshot) {
                        adjusted.afterSnapshot.push_back(next[static_cast<size_t>(
                            FindAnnotIndexByIdInList(next, a.id))]);
                    }
                    precision = std::move(reordered);
                } else {
                    precision.clear();
                }
                break;
            default:
                return false;
            }
            // The exact applier owns the one transactional annotation copy.
            // All subsequent commits are noexcept; a failed preflight/apply
            // cannot consume precision provenance or publish partial state.
            if (!ApplyAnnotCommandToList(annots, adjusted)) return false;
            precisions_.swap(precision);
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::vector<AnnotNumericPrecision> precisions_;
    bool supported_ = false;
};

} // namespace annot_stage_replay
