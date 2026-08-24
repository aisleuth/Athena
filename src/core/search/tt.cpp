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
    const auto requested = std::max<std::size_t>(1, bytes / sizeof(Entry));
    const auto capacity = floor_power_of_two(requested);
    if (entries_.size() == capacity) return;
    entries_.assign(capacity, Entry{});
    mask_ = capacity - 1;
    generation_ = 0;
}

void TranspositionTable::clear() noexcept {
    std::fill(entries_.begin(), entries_.end(), Entry{});
    generation_ = 0;
}

const TranspositionTable::Entry* TranspositionTable::probe(
    chess::zobrist::Key key) const noexcept {
    const auto& entry = entries_[static_cast<std::size_t>(key) & mask_];
    return entry.bound != Bound::None && entry.key == key ? &entry : nullptr;
}

void TranspositionTable::store(chess::zobrist::Key key, int depth,
                               int extensions_used, Score score, Bound bound,
                               chess::Move best_move, bool quiescence,
                               int quiescence_depth) noexcept {
    auto& entry = entries_[static_cast<std::size_t>(key) & mask_];
    const bool same_key = entry.bound != Bound::None && entry.key == key;
    bool replace = entry.bound == Bound::None ||
        entry.generation != generation_;
    if (same_key) {
        if (entry.quiescence != quiescence) {
            replace = !quiescence;
        } else if (quiescence) {
            replace = quiescence_depth >= entry.quiescence_depth ||
                bound == Bound::Exact;
        } else {
            replace = depth > entry.depth ||
                (depth == entry.depth &&
                 extensions_used <= entry.extensions_used) ||
                bound == Bound::Exact;
        }
    } else if (!replace) {
        const int incoming_quality = quiescence
            ? quiescence_depth
            : 256 + depth;
        const int existing_quality = entry.quiescence
            ? entry.quiescence_depth
            : 256 + entry.depth;
        replace = incoming_quality >= existing_quality;
    }
    if (replace) {
        entry.key = key;
        entry.score = score;
        entry.best_move = best_move;
        entry.depth = static_cast<std::int16_t>(std::clamp(
            depth, 0, static_cast<int>(std::numeric_limits<std::int16_t>::max())));
        entry.extensions_used = static_cast<std::uint8_t>(extensions_used);
        entry.quiescence_depth = static_cast<std::uint8_t>(std::clamp(
            quiescence_depth, 0, 255));
        entry.generation = generation_;
        entry.quiescence = quiescence;
        entry.bound = bound;
    }
}

} // namespace athena
