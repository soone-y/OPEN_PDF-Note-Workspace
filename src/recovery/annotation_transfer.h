#pragma once
#include "core/annot_stage_replay.h"
#include <limits>
#include <set>

namespace recovery {
// Transactional, additive transfer. Preserve every field and order; only an
// identity collision creates a new ID. Reserve incoming IDs before allocating.
[[nodiscard]] inline bool
AppendAnnotations(std::vector<Annotation> *destination,
                  const std::vector<Annotation> &source) {
  if (!destination || !annot_stage_replay::ValidList(*destination) ||
      !annot_stage_replay::ValidList(source))
    return false;
  auto next = *destination;
  std::set<std::wstring> existing, reserved;
  for (const auto &annotation : next)
    existing.insert(annotation.id);
  reserved = existing;
  for (const auto &annotation : source)
    reserved.insert(annotation.id);
  size_t suffix = 1;
  for (const auto &annotation : source) {
    auto copy = annotation;
    if (existing.count(copy.id)) {
      do {
        if (suffix == std::numeric_limits<size_t>::max())
          return false;
        copy.id = L"recovery-copy-" + std::to_wstring(suffix++);
      } while (reserved.count(copy.id));
    }
    existing.insert(copy.id);
    reserved.insert(copy.id);
    next.push_back(std::move(copy));
  }
  if (!annot_stage_replay::ValidList(next))
    return false;
  destination->swap(next);
  return true;
}
} // namespace recovery
