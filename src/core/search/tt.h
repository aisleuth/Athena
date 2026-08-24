#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "chess/move.h"
#include "chess/zobrist.h"
#include "evaluate.h"

namespace athena::core {

class TranspositionTable {
public:
    enum class Bound : std::uint8_t {
        None,
        Exact,
        Lower,
        Upper,
    };

    struct Entry {
        chess::zobrist::Key key = 0;
        Score score = SCORE_DRAW;
        chess::Move best_move{};
        std::int16_t depth = -1;
        std::uint8_t extensions_used = 0;
        std::uint8_t quiescence_depth = 0;
        std::uint8_t generation = 0;
        bool quiescence = false;
        Bound bound = Bound::None;

        bool covers_quiescence(int remaining_depth,
                               int current_extensions_used) const noexcept {
            return quiescence
                ? quiescence_depth >= remaining_depth
                : extensions_used <= current_extensions_used;
        }
    };

    explicit TranspositionTable(std::size_t megabytes = 16);

    void resize(std::size_t megabytes);
    void clear() noexcept;
    void new_search() noexcept {
        if (++generation_ == 0) {
            clear();
            generation_ = 1;
        }
    }

    const Entry* probe(chess::zobrist::Key key) const noexcept;
    void store(chess::zobrist::Key key, int depth, int extensions_used,
               Score score,
               Bound bound, chess::Move best_move,
               bool quiescence = false,
               int quiescence_depth = 0) noexcept;

    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<Entry> entries_;
    std::size_t mask_{0};
    std::uint8_t generation_{0};
};

} // namespace athena
