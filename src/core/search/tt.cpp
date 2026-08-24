#include "tt.h"
#include <algorithm>
#include <limits>

namespace athena::core {

namespace {

std::size_t floor_power_of_two(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result <= value / 2) result *= 2;
    return result;
}

} // namespace

TranspositionTable::TranspositionTable(std::size_t megabytes) {
    resize(megabytes);
}

void TranspositionTable::resize(std::size_t megabytes) {
    megabytes = std::max<std::size_t>(1, megabytes);
    const auto bytes = megabytes * 1024ULL * 1024ULL;
    const auto requested = std::max<std::size_t>(BUCKET_SIZE, bytes / sizeof(Entry));
    const auto capacity = floor_power_of_two(requested);
    if (entries_.size() == capacity) return;
    entries_.assign(capacity, Entry{});
    mask_ = (capacity - 1) & ~(BUCKET_SIZE - 1);
    generation_ = 0;
}

void TranspositionTable::clear() noexcept {
    std::fill(entries_.begin(), entries_.end(), Entry{});
    generation_ = 0;
}

const TranspositionTable::Entry* TranspositionTable::probe(
    chess::zobrist::Key key) const noexcept {
    const auto base = static_cast<std::size_t>(key) & mask_;
    for (std::size_t slot = 0; slot < BUCKET_SIZE; ++slot) {
        const auto& entry = entries_[base + slot];
        if (entry.bound != Bound::None && entry.key == key) return &entry;
    }
    return nullptr;
}

void TranspositionTable::store(chess::zobrist::Key key, int depth,
                               int extensions_used, Score score, Bound bound,
                               chess::Move best_move, bool quiescence,
                               int quiescence_depth) noexcept {
    const auto base = static_cast<std::size_t>(key) & mask_;

    // Prefer the slot already holding this key; otherwise evict the least
    // valuable slot in the bucket (empty or stale-generation first, then
    // main-search entries beat quiescence entries, then shallower loses).
    Entry* target = nullptr;
    Entry* victim = nullptr;
    int victim_quality = std::numeric_limits<int>::max();
    for (std::size_t slot = 0; slot < BUCKET_SIZE; ++slot) {
        auto& entry = entries_[base + slot];
        if (entry.bound != Bound::None && entry.key == key) {
            target = &entry;
            break;
        }
        const int quality = entry.bound == Bound::None
            ? std::numeric_limits<int>::min()
            : (entry.generation == generation_ ? 4096 : 0) +
              (entry.quiescence ? entry.quiescence_depth : 256 + entry.depth);
        if (quality < victim_quality) {
            victim_quality = quality;
            victim = &entry;
        }
    }

    bool replace = true;
    if (target != nullptr) {
        // Same-key refinement keeps the depth / extension-budget rules that
        // guard against path-dependent check-extension scores.
        if (target->quiescence != quiescence) {
            replace = !quiescence;
        } else if (quiescence) {
            replace = quiescence_depth >= target->quiescence_depth ||
                bound == Bound::Exact;
        } else {
            replace = depth > target->depth ||
                (depth == target->depth &&
                 extensions_used <= target->extensions_used) ||
                bound == Bound::Exact;
        }
    } else {
        target = victim;
    }
    if (!replace) return;

    target->key = key;
    target->score = score;
    target->best_move = best_move;
    target->depth = static_cast<std::int16_t>(std::clamp(
        depth, 0, static_cast<int>(std::numeric_limits<std::int16_t>::max())));
    target->extensions_used = static_cast<std::uint8_t>(extensions_used);
    target->quiescence_depth = static_cast<std::uint8_t>(std::clamp(
        quiescence_depth, 0, 255));
    target->generation = generation_;
    target->quiescence = quiescence;
    target->bound = bound;
}

} // namespace athena
