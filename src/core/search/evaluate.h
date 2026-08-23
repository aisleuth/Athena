#pragma once

#include <cstdint>
#include "chess/position.h"

namespace athena::core {

using Score = std::int32_t;

inline constexpr Score SCORE_INFINITE = 1'000'000;
inline constexpr Score SCORE_MATE = 100'000;
inline constexpr Score SCORE_DRAW = 0;

Score evaluate_material(const chess::Position& pos) noexcept;
Score evaluate_positional(const chess::Position& pos) noexcept;
Score evaluate(const chess::Position& pos) noexcept;

bool is_mate_score(Score score) noexcept;
int mate_in(Score score) noexcept;

} // namespace athena
