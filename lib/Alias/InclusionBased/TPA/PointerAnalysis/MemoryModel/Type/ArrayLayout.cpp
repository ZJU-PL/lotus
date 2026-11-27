// Implementation of ArrayLayout.
//
// ArrayLayout describes the regions within a type that correspond to arrays.
// This is critical for handling array indexing, where multiple concrete offsets
// need to be collapsed into a single "summary" offset to keep analysis finite.
//
// Data Structure:
// - A list of `ArrayTriple`s: {start, end, element_size}.
// - Example: struct { int x; int arr[10]; int y; }
//   - x: offset 0, size 4.
//   - arr: offset 4, size 40 (10 * 4). Triple: {4, 44, 4}.
//   - y: offset 44, size 4.
//
// Logic:
// - `offsetInto`: Checks if a raw byte offset falls within any array region.
//   If so, it normalizes the offset to the first element (modulo arithmetic).
//   e.g., accessing arr[3] (offset 4 + 3*4 = 16) -> maps to offset 4.

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/ArrayLayout.h"

#include <limits>
#include <mutex>
#include <numeric>

namespace tpa {

// Validates the integrity of the array triples list.
// - Intervals must be well-formed (start + size <= end).
// - Length must be a multiple of element size.
// - List must be sorted and non-overlapping.
static bool validateTripleList(const ArrayLayout::ArrayTripleList &list) {
  for (auto const &triple : list) {
    if (triple.start + triple.size > triple.end)
      return false;
    if ((triple.end - triple.start) % triple.size != 0)
      return false;
  }

  auto const isSorted = std::is_sorted(
      list.begin(), list.end(), [](auto const &lhs, auto const &rhs) {
        return (lhs.start < rhs.start) ||
               (lhs.start == rhs.start && lhs.size > rhs.size);
      });

  if (!isSorted)
    return false;

  return std::unordered_set<ArrayTriple>(list.begin(), list.end()).size() ==
         list.size();
}

const ArrayLayout *ArrayLayout::getLayout(ArrayTripleList &&list) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  assert(validateTripleList(list));
  auto itr = layoutSet.insert(ArrayLayout(std::move(list))).first;
  return &(*itr);
}

const ArrayLayout *
ArrayLayout::getLayout(std::initializer_list<ArrayTriple> ilist) {
  ArrayTripleList list(ilist);
  return getLayout(std::move(list));
}

// Layout for a generic byte array (char* or unknown size).
// Treats the entire range as one large array of bytes.
const ArrayLayout *ArrayLayout::getByteArrayLayout() {
  return getLayout({{0, std::numeric_limits<size_t>::max(), 1}});
}

const ArrayLayout *ArrayLayout::getDefaultLayout() { return defaultLayout; }

// Maps a raw offset to a normalized offset.
// Returns: {new_offset, is_array_access}
// If the offset falls into an array triple, returns the offset of the base
// element and true. Otherwise returns original offset and false.
std::pair<size_t, bool> ArrayLayout::offsetInto(size_t offset) const {
  bool hitArray = false;
  for (auto const &triple : arrayLayout) {
    if (triple.start > offset)
      break;

    if (triple.start <= offset && offset < triple.end) {
      hitArray = true;
      // Normalize: map to the start of the array + offset within the element.
      // Actually, here it maps to the specific element slot within the *first*
      // index? Logic: start + (off - start) % size. Example: start=0, size=4.
      // Access 8. 0 + (8-0)%4 = 0. Example: start=0, size=4. Access 9 (byte 1
      // of index 2). 0 + (9-0)%4 = 1. So it preserves intra-element offset but
      // collapses indices.
      offset = triple.start + (offset - triple.start) % triple.size;
    }
  }
  return std::make_pair(offset, hitArray);
}

std::vector<std::pair<size_t, bool>>
ArrayLayout::collectPeriodicOffsets(size_t base, size_t limit,
                                    size_t stride) const {
  std::vector<std::pair<size_t, bool>> out;
  if (base >= limit)
    return out;
  if (stride == 0) {
    out.push_back(offsetInto(base));
    return out;
  }

  auto collect = [&](auto &&self, size_t raw, size_t lim, size_t step,
                     size_t tripleIdx, bool hitArray) -> void {
    while (raw < lim) {
      while (tripleIdx < arrayLayout.size() &&
             (arrayLayout[tripleIdx].size == 0 ||
              arrayLayout[tripleIdx].end <= raw))
        ++tripleIdx;

      if (tripleIdx >= arrayLayout.size() ||
          arrayLayout[tripleIdx].start >= lim) {
        while (raw < lim) {
          out.emplace_back(raw, hitArray);
          if (lim - raw <= step)
            break;
          raw += step;
        }
        return;
      }

      const auto &triple = arrayLayout[tripleIdx];
      if (raw < triple.start) {
        const size_t prefixEnd = std::min(triple.start, lim);
        while (raw < prefixEnd) {
          out.emplace_back(raw, hitArray);
          if (lim - raw <= step)
            return;
          raw += step;
        }
        if (raw >= triple.end)
          continue;
      }

      const size_t effEnd = std::min(triple.end, lim);
      const size_t count = (effEnd - 1 - raw) / step + 1;
      const size_t g = std::gcd(step, triple.size);
      const size_t periodSteps = triple.size / g;

      if (count >= periodSteps) {
        const size_t firstRes = (raw - triple.start) % g;
        self(self, triple.start + firstRes, triple.start + triple.size, g,
             tripleIdx + 1, true);
      } else {
        size_t rem = count;
        size_t cur = triple.start + (raw - triple.start) % triple.size;
        const size_t elemEnd = triple.start + triple.size;
        while (rem > 0) {
          const size_t stepsInSeg =
              std::min(rem, (elemEnd - 1 - cur) / step + 1);
          const size_t segLimit = cur + (stepsInSeg - 1) * step + 1;
          self(self, cur, segLimit, step, tripleIdx + 1, true);
          rem -= stepsInSeg;
          if (rem > 0) {
            const size_t curOff = cur - triple.start;
            const size_t advance =
                ((stepsInSeg % triple.size) * (step % triple.size)) %
                triple.size;
            cur = triple.start + ((curOff + advance) % triple.size);
          }
        }
      }

      if (effEnd >= lim)
        break;
      const size_t skip = (effEnd - raw + step - 1) / step;
      if ((lim - 1 - raw) / step < skip)
        break;
      raw += skip * step;
    }
  };

  collect(collect, base, limit, stride, 0, false);
  return out;
}

} // namespace tpa