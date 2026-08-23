#pragma once

#include <cstdint>

namespace athena::chess {

class Position;

namespace zobrist {

using Key = std::uint64_t;

Key hash(const Position& position) noexcept;

} // namespace zobrist

} // namespace athena
