#include "zobrist.h"
#include <array>
#include <cstddef>
#include "bitboard.h"
#include "constants.h"
#include "piececolor.h"
#include "position.h"

namespace athena::chess::zobrist {

namespace {

constexpr Key splitmix64(Key value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

constexpr Key keyed_random(Key category, Key index) noexcept {
    return splitmix64(0xA7E4D3C2B1908F61ULL ^ (category << 48) ^ index);
}

} // namespace

Key hash(const Position& position) noexcept {
    Key key = 0;

    for (int piece_index = 0; piece_index < PIECE_NB; ++piece_index) {
        const auto piece = static_cast<Piece::ID>(piece_index);
        for (int color_index = 0; color_index < COLOR_NB; ++color_index) {
            const auto color = static_cast<Color::ID>(color_index);
            auto pieces = position.bitboard(piece) & position.bitboard(color);
            const auto piece_color = PieceColor(color, piece).id();
            while (pieces.any()) {
                const auto square = Bitboard::pop_lsb(pieces);
                const auto index = static_cast<Key>(piece_color) * SQUARE_NB
                    + static_cast<Key>(square.id());
                key ^= keyed_random(1, index);
            }
        }
    }

    key ^= keyed_random(2, static_cast<Key>(position.turn().id()));
    key ^= keyed_random(3, static_cast<Key>(position.state().castle.id()));
    key ^= keyed_random(4, static_cast<Key>(position.setup()));

    for (int color_index = 0; color_index < COLOR_NB; ++color_index) {
        const auto color = static_cast<Color::ID>(color_index);
        const auto square = position.enpass(color);
        if (square != Square::offboard()) {
            const auto index = static_cast<Key>(color_index) * SQUARE_NB
                + static_cast<Key>(square.id());
            key ^= keyed_random(5, index);
        }
    }

    return key;
}

} // namespace athena::chess::zobrist
