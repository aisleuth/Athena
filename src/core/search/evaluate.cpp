#include "evaluate.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include "chess/attacks.h"
#include "chess/psqt.h"

namespace athena::core {

namespace {

constexpr std::array<Score, chess::PIECE_NB> MATERIAL = {
    0,    // King: game-ending positions are handled by the search.
    300,  // Knight
    350,  // Bishop
    500,  // Rook
    1000, // Queen
    100,  // Pawn
};

// In team 4PC a check can constrain an opponent several plies before that
// player moves, giving the teammate time to reinforce the attack. Immediate
// checks are searched tactically; this term preserves some value for pressure
// against the other opposing king at a static horizon.
constexpr Score CHECK_PRESSURE = 120;

int centrality(chess::Square square) noexcept {
    const int file_distance = std::abs(2 * static_cast<int>(square.file()) - 15);
    const int rank_distance = std::abs(2 * static_cast<int>(square.rank()) - 15);
    return 26 - file_distance - rank_distance;
}

int pawn_advance(chess::Square square, chess::Color::ID color) noexcept {
    int advance = 0;
    switch (color) {
        case chess::Color::ID::Red:
            advance = static_cast<int>(square.rank()) - 2;
            break;
        case chess::Color::ID::Blue:
            advance = static_cast<int>(square.file()) - 2;
            break;
        case chess::Color::ID::Yellow:
            advance = 13 - static_cast<int>(square.rank());
            break;
        case chess::Color::ID::Green:
            advance = 13 - static_cast<int>(square.file());
            break;
        default:
            break;
    }
    return std::clamp(advance, 0, 10);
}

Score piece_position(chess::Piece::ID piece, chess::Square square,
                     chess::Color::ID color) noexcept {
    const int center = centrality(square);
    switch (piece) {
        case chess::Piece::ID::Knight: return 3 * center;
        case chess::Piece::ID::Bishop: return 2 * center;
        case chess::Piece::ID::Rook:   return center;
        case chess::Piece::ID::Queen:  return center;
        case chess::Piece::ID::Pawn:   return 8 * pawn_advance(square, color) + center;
        default:                       return 0;
    }
}

} // namespace

Score evaluate_material(const chess::Position& pos) noexcept {
    Score score = 0;

    for (std::size_t i = 0; i < MATERIAL.size(); ++i) {
        const auto piece = static_cast<chess::Piece::ID>(i);
        const auto pieces = pos.bitboard(piece);
        const auto friendly = (pieces & pos.teammate()).count();
        const auto enemy = (pieces & pos.opponent()).count();
        score += MATERIAL[i] * (friendly - enemy);
    }

    return score;
}

Score evaluate_positional(const chess::Position& pos) noexcept {
    Score score = 0;
    const auto perspective = pos.turn();

    for (int color_index = 0; color_index < chess::COLOR_NB; ++color_index) {
        const auto color = static_cast<chess::Color::ID>(color_index);
        const Score sign = perspective.same(color) ? 1 : -1;

        for (int piece_index = 0; piece_index < chess::PIECE_NB; ++piece_index) {
            const auto piece = static_cast<chess::Piece::ID>(piece_index);
            auto pieces = pos.bitboard(piece) & pos.bitboard(color);
            while (pieces.any()) {
                const auto square = chess::Bitboard::pop_lsb(pieces);
                score += sign * piece_position(piece, square, color);
            }
        }

        // Pawns belonging to either teammate help shelter this king.
        const auto ally = chess::Color(color).ally().id();
        const auto team = pos.bitboard(color) | pos.bitboard(ally);
        const auto shield = chess::get_crawl_attacks<chess::Piece::ID::King>(
            pos.royal(color)) & pos.bitboard(chess::Piece::ID::Pawn) & team;
        score += sign * 15 * shield.count();

        if (pos.in_check(color)) score -= sign * CHECK_PRESSURE;
    }

    return score;
}

Score evaluate(const chess::Position& pos) noexcept {
    // Material and piece-square terms are maintained incrementally by
    // Position (see chess/psqt.h); only the dynamic terms are computed here.
    const auto perspective = pos.turn();
    Score score = pos.psq(perspective.id()) + pos.psq(perspective.ally().id())
        - pos.psq(perspective.next().id()) - pos.psq(perspective.prev().id());

    // Teams are the even and odd colours (Red+Yellow, Blue+Green), so the two
    // team masks and the pawn mask are shared across the four kings instead of
    // being rebuilt per colour.
    const auto& pawns = pos.bitboard(chess::Piece::ID::Pawn);
    const chess::Bitboard team_mask[2] = {
        pos.bitboard(chess::Color::ID::Red) | pos.bitboard(chess::Color::ID::Yellow),
        pos.bitboard(chess::Color::ID::Blue) | pos.bitboard(chess::Color::ID::Green),
    };

    for (int color_index = 0; color_index < chess::COLOR_NB; ++color_index) {
        const auto color = static_cast<chess::Color::ID>(color_index);
        const Score sign = perspective.same(color) ? 1 : -1;

        // Pawns belonging to either teammate help shelter this king.
        const auto shield = chess::get_crawl_attacks<chess::Piece::ID::King>(
            pos.royal(color)) & pawns & team_mask[color_index & 1];
        score += sign * 15 * shield.count();

        if (pos.in_check(color)) score -= sign * CHECK_PRESSURE;
    }

    return score;
}

bool is_mate_score(Score score) noexcept {
    return std::abs(score) >= SCORE_MATE - 512;
}

int mate_in(Score score) noexcept {
    const auto plies = SCORE_MATE - std::abs(score);
    return score >= 0 ? (plies + 1) / 2 : -(plies / 2);
}

} // namespace athena
