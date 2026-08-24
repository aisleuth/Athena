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

template <std::size_t Size>
constexpr std::array<Key, Size> make_key_table(Key category) noexcept {
    std::array<Key, Size> result{};
    for (std::size_t index = 0; index < Size; ++index) {
        result[index] = keyed_random(category, static_cast<Key>(index));
    }
    return result;
}

constexpr auto PIECE_KEYS = make_key_table<PIECECOLOR_NB * SQUARE_NB>(1);
constexpr auto TURN_KEYS = make_key_table<COLOR_NB>(2);
constexpr auto CASTLE_KEYS = make_key_table<CASTLE_NB>(3);
constexpr auto SETUP_KEYS = make_key_table<SETUP_NB>(4);
constexpr auto ENPASSANT_KEYS = make_key_table<COLOR_NB * SQUARE_NB>(5);

} // namespace

Key piece_square(PieceColor piece, Square square) noexcept {
    const auto index = static_cast<Key>(piece.id()) * SQUARE_NB
        + static_cast<Key>(square.id());
    return PIECE_KEYS[static_cast<std::size_t>(index)];
}

Key turn(Color::ID color) noexcept {
    return TURN_KEYS[static_cast<std::size_t>(color)];
}

Key castle(Castle rights) noexcept {
    return CASTLE_KEYS[static_cast<std::size_t>(rights.id())];
}

Key setup(Castle::Setup value) noexcept {
    return SETUP_KEYS[static_cast<std::size_t>(value)];
}

Key enpassant(Color::ID color, Square square) noexcept {
    const auto index = static_cast<Key>(color) * SQUARE_NB
        + static_cast<Key>(square.id());
    return ENPASSANT_KEYS[static_cast<std::size_t>(index)];
}

Key hash(const Position& position) noexcept {
    return position.key();
}

Key recompute(const Position& position) noexcept {
    Key key = 0;

    for (int piece_index = 0; piece_index < PIECE_NB; ++piece_index) {
        const auto piece = static_cast<Piece::ID>(piece_index);
        for (int color_index = 0; color_index < COLOR_NB; ++color_index) {
            const auto color = static_cast<Color::ID>(color_index);
            auto pieces = position.bitboard(piece) & position.bitboard(color);
            const auto piece_color = PieceColor(color, piece).id();
            while (pieces.any()) {
                const auto square = Bitboard::pop_lsb(pieces);
                key ^= piece_square(PieceColor(piece_color), square);
            }
        }
    }

    key ^= turn(position.turn().id());
    key ^= castle(position.state().castle);
    key ^= setup(position.setup());

    for (int color_index = 0; color_index < COLOR_NB; ++color_index) {
        const auto color = static_cast<Color::ID>(color_index);
        const auto square = position.enpass(color);
        if (square != Square::offboard()) {
            key ^= enpassant(color, square);
        }
    }

    return key;
}

} // namespace athena::chess::zobrist
