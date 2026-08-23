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
    entries_.assign(capacity, Entry{});
    mask_ = capacity - 1;
}

void TranspositionTable::clear() noexcept {
    std::fill(entries_.begin(), entries_.end(), Entry{});
}

const TranspositionTable::Entry* TranspositionTable::probe(
    chess::zobrist::Key key) const noexcept {
    const auto& entry = entries_[static_cast<std::size_t>(key) & mask_];
    return entry.bound != Bound::None && entry.key == key ? &entry : nullptr;
}

void TranspositionTable::store(chess::zobrist::Key key, int depth,
                               int extensions_used, Score score, Bound bound,
                               chess::Move best_move) noexcept {
    auto& entry = entries_[static_cast<std::size_t>(key) & mask_];
    if (entry.key != key || depth > entry.depth ||
        (depth == entry.depth && extensions_used <= entry.extensions_used) ||
        bound == Bound::Exact) {
        entry.key = key;
        entry.score = score;
        entry.best_move = best_move;
        entry.depth = static_cast<std::int16_t>(std::clamp(
            depth, 0, static_cast<int>(std::numeric_limits<std::int16_t>::max())));
        entry.extensions_used = static_cast<std::uint8_t>(extensions_used);
        entry.bound = bound;
    }
}

} // namespace athena
