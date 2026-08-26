#pragma once

#include "constants.h"
#include "move.h"
#include "piececolor.h"
#include "bitboard.h"
#include "square.h"
#include "castle.h"
#include <array>
#include "psqt.h"
#include "zobrist.h"
#include <cstdint>

namespace athena::chess {

class alignas(64) Position {
public:
    using FEN   = std::string;
    using Clock = uint16_t;
    using Depth = uint16_t;

    struct alignas(4) State
    {
        PieceColor capture;
        Square     enpass;
        Castle     castle;
        Clock      fifty_move_clock;
    };

    Position() noexcept = default;

    void init(const FEN& fen) noexcept;
    FEN fen() const noexcept;
    void print(bool board16x16 = false) const;

    void set_setup(Castle::Setup setup) noexcept { setup_ = setup; }
    auto setup() const noexcept { return setup_; }

    const Bitboard& bitboard(Piece::ID piece) const noexcept { return piece_[static_cast<uint8_t>(piece)]; } // return refence
    const Bitboard& bitboard(Color::ID color) const noexcept { return color_[static_cast<uint8_t>(color)]; }

    auto enpass(Color::ID color) const noexcept { return enpass_[static_cast<uint8_t>(color)]; }
    auto royal( Color::ID color) const noexcept { return  royal_[static_cast<uint8_t>(color)]; }

    auto turn() const noexcept { return turn_; }
    auto psq(Color::ID color) const noexcept { return psq_[static_cast<uint8_t>(color)]; }
    auto play() const noexcept { return play_; }
    auto key() const noexcept { return key_; }

    bool is_repetition(int required_occurrences = 3) const noexcept;
    bool is_fifty_move_draw() const noexcept {
        // In 4PC one complete move consists of all four players taking a turn.
        return state().fifty_move_clock >= 4 * 50;
    }

    void set_board(Square sq, PieceColor pc) noexcept;
    void pop_board(Square sq, PieceColor pc) noexcept;

    auto board(Square::ID sq) const noexcept { return board_[static_cast<uint8_t>(sq)]; }

    void make_move(Move move) noexcept;
    void undo_move(Move move) noexcept;

    void make_move(const std::string& move, bool board16x16 = false) noexcept;

    // Bitboard teammate() const noexcept { return bitboard(turn_.self().id()) | bitboard(turn_.ally().id()); }
    // Bitboard opponent() const noexcept { return bitboard(turn_.next().id()) | bitboard(turn_.prev().id()); }
    // Bitboard occupied() const noexcept { return teammate() | opponent(); }

    bool legal(Move* move) const noexcept;
    Bitboard get_attackers_bitboard(Square sq, Color color, const Bitboard& occupancy) const noexcept;

    // Early-exit variant of get_attackers_bitboard: answers "is sq attacked by
    // an opponent of color?" without materializing the full attacker set, and
    // skips the sliding scans when no enemy slider is aligned with sq on an
    // empty board. `exclude` masks attackers off (used by would_check to
    // ignore the moving piece, which the bitboards still hold at its source).
    bool attacked(Square sq, Color color, const Bitboard& occupancy,
                  const Bitboard& exclude = Bitboard{}) const noexcept;

    // Exact equivalent of { make_move(move); in_check(king_color);
    // undo_move(move); } for a pseudo-legal move by the side to move and an
    // opposing king, without touching the position. Includes checks the move
    // leaves standing, discovered checks, promotions, en passant, castling.
    bool would_check(Move move, Color::ID king_color) const noexcept;

    // Precomputed per-node data for answering "does this move check `king`?"
    // without touching the board. Built once per node by init_check_info and
    // consumed by gives_check for every move at that node.
    struct CheckInfo {
        // Uninitialised by design: init_check_info writes discovery
        // unconditionally and check_squares whenever usable is set.
        std::array<Bitboard, PIECE_NB> check_squares;
        Bitboard discovery;     // side-to-move pieces that may discover check
        Square king{};
        Color::ID king_color{Color::ID::None};
        bool usable{false};     // false => caller must use would_check
    };

    void init_check_info(Color::ID king_color, CheckInfo& info) const noexcept;

    // Exactly equivalent to would_check(move, info.king_color), but O(1) in
    // bitboard ops for the common move policies. Falls back to would_check
    // for castling, en passant, and promotions.
    bool gives_check(Move move, const CheckInfo& info) const noexcept;

    // Check status is queried repeatedly for the same position: once by the
    // search, four times by evaluate's check-pressure term, and once per
    // opposing king by init_check_info. Memoise against the incremental
    // Zobrist key, which every board mutation already updates -- that makes
    // the cache self-invalidating with no work in make_move/undo_move.
    bool in_check(Color::ID color) const noexcept {
        if (check_cache_key_ != key_) {
            check_cache_key_ = key_;
            check_cache_.fill(-1);
        }
        auto& slot = check_cache_[static_cast<uint8_t>(color)];
        if (slot < 0) {
            slot = attacked(royal(color), Color(color), occupied()) ? 1 : 0;
        }
        return slot != 0;
    }

    bool in_check() const noexcept { return in_check(turn_.id()); }
    bool consistent() const noexcept;

    const Bitboard& teammate() const noexcept { return occupancy_[play_][0]; }
    const Bitboard& opponent() const noexcept { return occupancy_[play_][1]; }
    const Bitboard& occupied() const noexcept { return occupancy_[play_][2]; }

    // Bitboard get_pinned_bitboard(
    //     const Bitboard& teammate, 
    //     const Bitboard& opponent,
    //     const Bitboard& occupied,
    //     const Bitboard& us) const noexcept;

          void  set_pinned_checks_bitboards()       noexcept;
          auto& get_pinned_checks_bitboards()       noexcept { return pinned_checks_[play_]; }
    const auto& get_pinned_checks_bitboards() const noexcept { return pinned_checks_[play_]; }

    Position::State& state() noexcept { return state_[play_]; }
    const Position::State& state() const noexcept { return state_[play_]; }

private:
    alignas(64) std::array<std::pair<Bitboard, Bitboard>, PLAY_NB> pinned_checks_;
    alignas(64) std::array<PieceColor, SQUARE_NB> board_;
    alignas(64) std::array<Bitboard  , PIECE_NB > piece_;
    alignas(64) std::array<Bitboard  , COLOR_NB > color_;
    alignas(64) std::array<State     , PLAY_NB  > state_;
    alignas(64) std::array<std::array<Bitboard, 3>, PLAY_NB> occupancy_;
    std::array<Square, COLOR_NB> enpass_;
    std::array<Square, COLOR_NB> royal_;
    std::array<zobrist::Key, PLAY_NB> key_history_{};
    std::array<psqt::Value, COLOR_NB + 1> psq_{}; // +1: None slot absorbs stray writes
    mutable zobrist::Key check_cache_key_{~zobrist::Key{0}};
    mutable std::array<std::int8_t, COLOR_NB> check_cache_{-1, -1, -1, -1};
    zobrist::Key key_{0};
    Depth play_{0};
    Color turn_{Color::ID::Red};
    Castle::Setup setup_{Castle::Setup::Modern};

    void set_enpass(Square sq, Color::ID color) noexcept { enpass_[static_cast<uint8_t>(color)] = sq; }
    void set_royal( Square sq, Color::ID color) noexcept {  royal_[static_cast<uint8_t>(color)] = sq; }

    void set_occupany_bitboards() noexcept { 
        occupancy_[play_][0] = bitboard(turn_.self().id()) | bitboard(turn_.ally().id());
        occupancy_[play_][1] = bitboard(turn_.next().id()) | bitboard(turn_.prev().id());
        occupancy_[play_][2] = occupancy_[play_][0] | occupancy_[play_][1];
    }

    void clear_state() noexcept {
        play_ = 0;
        auto& state = state_[play_];
        state.capture = PieceColor::empty();
        state.enpass  = Square::offboard();
        state.castle  = 0;
        state.fifty_move_clock = 0;
        key_ = 0;
        key_history_.fill(0);
    }

    void clear_board() noexcept {
        psq_.fill(0);
        enpass_.fill(Square::offboard());
        royal_.fill(Square::offboard());
        piece_.fill(Bitboard(0ULL));
        color_.fill(Bitboard(0ULL));

        for (int sq = 0; sq < SQUARE_NB; ++sq) {
            board_[static_cast<std::size_t>(sq)] =
                Square(static_cast<Square::ID>(sq)).isStone()
                       ? PieceColor::stone()
                       : PieceColor::empty();
        }
    }

private:
    inline static const std::array<FEN, SETUP_NB> STARTPOS = 
    {
        FEN{
            "R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-0-"
            "x,x,x,yR,yN,yB,yK,yQ,yB,yN,yR,x,x,x/"
            "x,x,x,yP,yP,yP,yP,yP,yP,yP,yP,x,x,x/"
            "x,x,x,8,x,x,x/"
            "bR,bP,10,gP,gR/"
            "bN,bP,10,gP,gN/"
            "bB,bP,10,gP,gB/"
            "bQ,bP,10,gP,gK/"
            "bK,bP,10,gP,gQ/"
            "bB,bP,10,gP,gB/"
            "bN,bP,10,gP,gN/"
            "bR,bP,10,gP,gR/"
            "x,x,x,8,x,x,x/"
            "x,x,x,rP,rP,rP,rP,rP,rP,rP,rP,x,x,x/"
            "x,x,x,rR,rN,rB,rQ,rK,rB,rN,rR,x,x,x"
        },

        FEN{
            "R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-0-"
            "x,x,x,yR,yN,yB,yK,yQ,yB,yN,yR,x,x,x/"
            "x,x,x,yP,yP,yP,yP,yP,yP,yP,yP,x,x,x/"
            "x,x,x,8,x,x,x/"
            "bR,bP,10,gP,gR/"
            "bN,bP,10,gP,gN/"
            "bB,bP,10,gP,gB/"
            "bK,bP,10,gP,gQ/"
            "bQ,bP,10,gP,gK/"
            "bB,bP,10,gP,gB/"
            "bN,bP,10,gP,gN/"
            "bR,bP,10,gP,gR/"
            "x,x,x,8,x,x,x/"
            "x,x,x,rP,rP,rP,rP,rP,rP,rP,rP,x,x,x/"
            "x,x,x,rR,rN,rB,rQ,rK,rB,rN,rR,x,x,x"
        }
    };

public:
    static std::string startpos(Castle::Setup setup) noexcept {
        return STARTPOS[static_cast<uint8_t>(setup)];
    }
};

} // namespace athena
