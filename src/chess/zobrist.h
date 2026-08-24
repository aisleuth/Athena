#pragma once

#include <cstdint>
#include "castle.h"
#include "color.h"
#include "piececolor.h"
#include "square.h"

namespace athena::chess {

class Position;

namespace zobrist {

using Key = std::uint64_t;

Key hash(const Position& position) noexcept;
Key recompute(const Position& position) noexcept;
Key piece_square(PieceColor piece, Square square) noexcept;
Key turn(Color::ID color) noexcept;
Key castle(Castle rights) noexcept;
Key setup(Castle::Setup setup) noexcept;
Key enpassant(Color::ID color, Square square) noexcept;

} // namespace zobrist

} // namespace athena
