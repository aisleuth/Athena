#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "constants.h"
#include "color.h"
#include "piece.h"
#include "square.h"

namespace athena::chess::psqt {

// Static material + piece-square values, indexed [piece][color][square].
//
// The table lives in the chess layer so Position can maintain an incremental
// per-color accumulator inside set_board/pop_board (the single choke point
// for every board mutation). Core evaluation folds the accumulators with the
// dynamic terms (king shelter, check pressure); the values here must stay in
// lockstep with the intent documented in core/search/evaluate.cpp.

using Value = std::int32_t;

// King material is zero: game-ending positions are handled by the search.
inline constexpr std::array<Value, PIECE_NB> MATERIAL = {
    0,    // King
    300,  // Knight
    350,  // Bishop
    500,  // Rook
    1000, // Queen
    100,  // Pawn
};

namespace detail {

inline int centrality(Square square) noexcept {
    const int file_distance = std::abs(2 * static_cast<int>(square.file()) - 15);
    const int rank_distance = std::abs(2 * static_cast<int>(square.rank()) - 15);
    return 26 - file_distance - rank_distance;
}

inline int pawn_advance(Square square, Color::ID color) noexcept {
    int advance = 0;
    switch (color) {
        case Color::ID::Red:
            advance = static_cast<int>(square.rank()) - 2;
            break;
        case Color::ID::Blue:
            advance = static_cast<int>(square.file()) - 2;
            break;
        case Color::ID::Yellow:
            advance = 13 - static_cast<int>(square.rank());
            break;
        case Color::ID::Green:
            advance = 13 - static_cast<int>(square.file());
            break;
        default:
            break;
    }
    return std::clamp(advance, 0, 10);
}

inline Value piece_position(Piece::ID piece, Square square,
                            Color::ID color) noexcept {
    const int center = centrality(square);
    switch (piece) {
        case Piece::ID::Knight: return 3 * center;
        case Piece::ID::Bishop: return 2 * center;
        case Piece::ID::Rook:   return center;
        case Piece::ID::Queen:  return center;
        case Piece::ID::Pawn:
            return 8 * pawn_advance(square, color) + center;
        default:                return 0;
    }
}

} // namespace detail

// Sized 8 x 5 so Piece::ID::Empty/Stone and Color::ID::None index harmless
// zero rows instead of reading out of bounds.
inline const auto TABLE = [] {
    std::array<std::array<std::array<Value, SQUARE_NB>, COLOR_NB + 1>, 8> table{};
    for (int piece = 0; piece < PIECE_NB; ++piece) {
        for (int color = 0; color < COLOR_NB; ++color) {
            for (int square = 0; square < SQUARE_NB; ++square) {
                const auto sq = Square(static_cast<Square::ID>(square));
                table[static_cast<std::size_t>(piece)]
                     [static_cast<std::size_t>(color)]
                     [static_cast<std::size_t>(square)] =
                    MATERIAL[static_cast<std::size_t>(piece)] +
                    detail::piece_position(static_cast<Piece::ID>(piece), sq,
                                           static_cast<Color::ID>(color));
            }
        }
    }
    return table;
}();

inline Value value(Piece::ID piece, Color::ID color, Square square) noexcept {
    return TABLE[static_cast<std::size_t>(piece)]
                [static_cast<std::size_t>(color)]
                [static_cast<std::size_t>(square.id())];
}

} // namespace athena::chess::psqt
