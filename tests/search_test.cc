#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include "chess/attacks.h"
#include "chess/constants.h"
#include "chess/movegen.h"
#include "chess/position.h"
#include "chess/zobrist.h"
#include "core/search/evaluate.h"
#include "core/search/search.h"

using namespace athena;

namespace {

chess::Position modern_start_position() {
    chess::Position position;
    position.set_setup(chess::Castle::Setup::Modern);
    position.init(chess::Position::startpos(chess::Castle::Setup::Modern));
    return position;
}

void play_uci(chess::Position& position, const std::string& uci) {
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    for (int index = 0; index < count; ++index) {
        if (moves[index].uci() == uci) {
            position.make_move(moves[index]);
            return;
        }
    }
    FAIL() << "Illegal test move: " << uci;
}

struct HashWalkStats {
    std::uint64_t nodes = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t castles = 0;
    std::uint64_t en_passants = 0;
};

void verify_hash_tree(chess::Position& position, int depth,
                      HashWalkStats& stats) {
    ++stats.nodes;
    if (position.key() != chess::zobrist::recompute(position)) {
        ++stats.mismatches;
    }
    if (depth == 0) return;

    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    for (int index = 0; index < count; ++index) {
        const auto move = moves[index];
        if (move.policy() == chess::Move::Policy::Castle) ++stats.castles;
        if (move.policy() == chess::Move::Policy::Enpass) ++stats.en_passants;
        const auto original_key = position.key();
        position.make_move(move);
        verify_hash_tree(position, depth - 1, stats);
        position.undo_move(move);
        if (position.key() != original_key) ++stats.mismatches;
    }
}

struct FastPathStats {
    std::uint64_t nodes = 0;
    std::uint64_t moves = 0;
    std::uint64_t attack_mismatches = 0;
    std::uint64_t check_mismatches = 0;
    std::uint64_t evaluation_mismatches = 0;
    std::uint64_t castles = 0;
    std::uint64_t en_passants = 0;
    std::uint64_t promotions = 0;
};

void verify_fast_paths_tree(chess::Position& position, int depth,
                            FastPathStats& stats) {
    ++stats.nodes;
    const auto full_evaluation = core::evaluate_material(position) +
        core::evaluate_positional(position);
    if (core::evaluate(position) != full_evaluation) {
        ++stats.evaluation_mismatches;
    }
    for (int color_index = 0; color_index < chess::COLOR_NB; ++color_index) {
        const auto color = static_cast<chess::Color::ID>(color_index);
        const auto king = position.royal(color);
        const auto expected = position.get_attackers_bitboard(
            king, chess::Color(color), position.occupied()).any();
        if (position.attacked(king, chess::Color(color),
                              position.occupied()) != expected) {
            ++stats.attack_mismatches;
        }
    }
    if (depth == 0) return;

    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    for (int index = 0; index < count; ++index) {
        const auto move = moves[index];
        const auto mover = position.turn();
        const auto next = mover.next().id();
        const auto previous = mover.prev().id();
        const bool predicts_next = position.would_check(move, next);
        const bool predicts_previous = position.would_check(move, previous);
        ++stats.moves;
        if (move.policy() == chess::Move::Policy::Castle) ++stats.castles;
        if (move.policy() == chess::Move::Policy::Enpass) ++stats.en_passants;
        if (move.policy() == chess::Move::Policy::Evolve) ++stats.promotions;

        position.make_move(move);
        const bool checks_next = position.get_attackers_bitboard(
            position.royal(next), chess::Color(next), position.occupied()).any();
        const bool checks_previous = position.get_attackers_bitboard(
            position.royal(previous), chess::Color(previous),
            position.occupied()).any();
        if (predicts_next != checks_next) ++stats.check_mismatches;
        if (predicts_previous != checks_previous) ++stats.check_mismatches;
        verify_fast_paths_tree(position, depth - 1, stats);
        position.undo_move(move);
    }
}

chess::Position castling_test_position() {
    chess::Position position;
    position.set_setup(chess::Castle::Setup::Modern);
    position.init(
        "R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-0-"
        "x,x,x,yR,2,yK,3,yR,x,x,x/"
        "x,x,x,yP,yP,yP,yP,yP,yP,yP,yP,x,x,x/"
        "x,x,x,8,x,x,x/"
        "bR,bP,10,gP,gR/1,bP,10,gP,1/1,bP,10,gP,1/"
        "1,bP,10,gP,gK/bK,bP,10,gP,1/1,bP,10,gP,1/"
        "1,bP,10,gP,1/bR,bP,10,gP,gR/"
        "x,x,x,8,x,x,x/"
        "x,x,x,rP,rP,rP,rP,rP,rP,rP,rP,x,x,x/"
        "x,x,x,rR,3,rK,2,rR,x,x,x");
    return position;
}

chess::Position coordinated_mate_position() {
    auto position = modern_start_position();
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = position.board(id);
        if (piece != chess::PieceColor::empty() &&
            piece != chess::PieceColor::stone()) {
            position.pop_board(chess::Square(id), piece);
        }
    }

    const auto place = [&position](const char* square, chess::Color::ID color,
                                   chess::Piece::ID piece) {
        position.set_board(chess::Square(square), chess::PieceColor(color, piece));
    };

    place("c6", chess::Color::ID::Red, chess::Piece::ID::King);
    place("b4", chess::Color::ID::Red, chess::Piece::ID::Rook);
    place("b8", chess::Color::ID::Blue, chess::Piece::ID::King);
    place("h14", chess::Color::ID::Yellow, chess::Piece::ID::King);
    place("n8", chess::Color::ID::Green, chess::Piece::ID::King);

    // Green's pieces take away every flight square around the Blue king. Red
    // can capture the b7 blocker with check, while the Red king protects b7.
    for (const char* square : {"a7", "a8", "a9", "b7", "b9",
                               "c7", "c8", "c9"}) {
        place(square, chess::Color::ID::Green, chess::Piece::ID::Pawn);
    }

    position.init(position.fen());
    return position;
}

chess::Position quiet_check_position() {
    auto position = modern_start_position();
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = position.board(id);
        if (piece != chess::PieceColor::empty() &&
            piece != chess::PieceColor::stone()) {
            position.pop_board(chess::Square(id), piece);
        }
    }

    position.set_board(chess::Square("h2"),
        chess::PieceColor(chess::Color::ID::Red, chess::Piece::ID::King));
    position.set_board(chess::Square("b8"),
        chess::PieceColor(chess::Color::ID::Blue, chess::Piece::ID::King));
    position.set_board(chess::Square("g10"),
        chess::PieceColor(chess::Color::ID::Blue, chess::Piece::ID::Rook));
    position.set_board(chess::Square("h14"),
        chess::PieceColor(chess::Color::ID::Yellow, chess::Piece::ID::King));
    position.set_board(chess::Square("n8"),
        chess::PieceColor(chess::Color::ID::Green, chess::Piece::ID::King));
    position.init(position.fen());
    return position;
}

TEST(EvaluateTest, StartingTeamsAreBalanced) {
    const auto position = modern_start_position();
    EXPECT_EQ(core::evaluate(position), core::SCORE_DRAW);
    EXPECT_EQ(core::evaluate_positional(position), core::SCORE_DRAW);
}

TEST(EvaluateTest, ScoresMaterialForTheSideToMoveTeam) {
    auto position = modern_start_position();
    bool removed = false;
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = position.board(id);
        if (piece == chess::PieceColor(chess::Color::ID::Blue,
                                      chess::Piece::ID::Queen)) {
            position.pop_board(chess::Square(id), piece);
            removed = true;
            break;
        }
    }

    ASSERT_TRUE(removed);
    EXPECT_EQ(core::evaluate_material(position), 1000);

    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    ASSERT_GT(count, 0);
    position.make_move(moves[0]);
    EXPECT_EQ(core::evaluate_material(position), -1000);
}

TEST(EvaluateTest, RewardsCentralPieceActivity) {
    auto position = modern_start_position();
    const auto baseline = core::evaluate_positional(position);
    ASSERT_EQ(position.board(chess::Square("h8").id()),
              chess::PieceColor::empty());

    bool moved = false;
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = position.board(id);
        if (piece == chess::PieceColor(chess::Color::ID::Red,
                                      chess::Piece::ID::Knight)) {
            position.pop_board(chess::Square(id), piece);
            position.set_board(chess::Square("h8"), piece);
            moved = true;
            break;
        }
    }

    ASSERT_TRUE(moved);
    position.init(position.fen());
    EXPECT_GT(core::evaluate_positional(position), baseline);
}

namespace {

// Walks the tree and asserts the O(1) gives_check fast path agrees with
// actually playing the move, for both opposing kings at every node.
void VerifyGivesCheck(chess::Position& position, int depth,
                      long long& tested, long long& fast) {
    chess::Move moves[chess::MOVE_NB];
    const int move_count = chess::generate_legal_moves(position, moves);

    for (int which = 0; which < 2; ++which) {
        const auto king_color = which ? position.turn().prev().id()
                                      : position.turn().next().id();
        chess::Position::CheckInfo info;
        position.init_check_info(king_color, info);

        for (int index = 0; index < move_count; ++index) {
            const auto move = moves[index];
            const bool predicted = position.gives_check(move, info);
            position.make_move(move);
            const bool actual = position.in_check(king_color);
            position.undo_move(move);

            ++tested;
            if (info.usable &&
                (move.policy() == chess::Move::Policy::Normal ||
                 move.policy() == chess::Move::Policy::Stride)) {
                ++fast;
            }
            ASSERT_EQ(predicted, actual)
                << "gives_check disagreed for " << move.uci()
                << " against king " << static_cast<int>(king_color)
                << " in " << position.fen();
        }
    }

    if (depth == 0) return;
    for (int index = 0; index < move_count; ++index) {
        position.make_move(moves[index]);
        VerifyGivesCheck(position, depth - 1, tested, fast);
        position.undo_move(moves[index]);
    }
}

} // namespace

TEST(PositionTest, GivesCheckMatchesPlayingTheMove) {
    chess::Position position;
    position.init(chess::Position::startpos(chess::Castle::Setup::Modern));

    long long tested = 0;
    long long fast = 0;
    VerifyGivesCheck(position, 2, tested, fast);

    EXPECT_GT(tested, 10'000);
    // The fast path must actually be carrying the traffic, not silently
    // degrading into the would_check fallback for everything.
    EXPECT_GT(fast, tested * 9 / 10);
}

TEST(PositionTest, GivesCheckHandlesCastlingAndPromotion) {
    // Castling, en passant, and promotions take the would_check fallback;
    // this pins that the fallback is wired up and still exact.
    chess::Position position;
    position.init(
        "R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-0-"
        "x,x,x,yR,2,yK,3,yR,x,x,x/"
        "x,x,x,yP,yP,yP,yP,yP,yP,yP,yP,x,x,x/"
        "x,x,x,8,x,x,x/"
        "bR,bP,10,gP,gR/1,bP,10,gP,1/1,bP,10,gP,1/1,bP,10,gP,gK/"
        "bK,bP,10,gP,1/1,bP,10,gP,1/1,bP,10,gP,1/bR,bP,10,gP,gR/"
        "x,x,x,8,x,x,x/"
        "x,x,x,rP,rP,rP,rP,rP,rP,rP,rP,x,x,x/"
        "x,x,x,rR,3,rK,2,rR,x,x,x");

    long long tested = 0;
    long long fast = 0;
    VerifyGivesCheck(position, 1, tested, fast);
    EXPECT_GT(tested, 1'000);
}

TEST(PositionTest, KingMayNotStepIntoAPawnAttack) {
    // Regression: get_pawn_attacks used to swap the Blue and Yellow attack
    // patterns, making checks by red and green pawns invisible. A green king
    // on m8 could then step into l7, attacked by the red pawn on k6, and be
    // captured on the next turn.
    chess::Position position;
    position.init(
        "G-0,0,0,0-0,0,0,0-0,0,0,0-0,0,0,0-0-"
        "x,x,x,8,x,x,x/"
        "x,x,x,2,yK,5,x,x,x/"
        "x,x,x,8,x,x,x/"
        "14/14/14/"
        "12,gK,1/"
        "1,bK,12/"
        "10,rP,3/"
        "14/14/"
        "x,x,x,8,x,x,x/"
        "x,x,x,8,x,x,x/"
        "x,x,x,2,rK,5,x,x,x");

    EXPECT_TRUE(position.attacked(chess::Square("l7"),
        chess::Color(chess::Color::ID::Green), position.occupied()));
    EXPECT_TRUE(position.attacked(chess::Square("j7"),
        chess::Color(chess::Color::ID::Green), position.occupied()));

    chess::Move moves[chess::MOVE_NB];
    const int move_count = chess::generate_legal_moves(position, moves);
    ASSERT_GT(move_count, 0);
    for (int index = 0; index < move_count; ++index) {
        EXPECT_NE(moves[index].uci(), "m8l7")
            << "green king stepped into the red pawn's attack";
    }
}

TEST(PositionTest, PawnAttackTablesFollowEveryColorDirection) {
    const chess::Square source("h8");
    for (int color_index = 0; color_index < chess::COLOR_NB; ++color_index) {
        const auto color = static_cast<chess::Color::ID>(color_index);
        const auto attacks = chess::get_pawn_attacks(source, color);
        EXPECT_TRUE(attacks.has_bit(source + chess::Square::take(color, 0)))
            << color_index;
        EXPECT_TRUE(attacks.has_bit(source + chess::Square::take(color, 1)))
            << color_index;
        EXPECT_EQ(attacks.count(), 2) << color_index;
    }
}

TEST(PositionTest, NoisyGeneratorMatchesFilteredLegalMoves) {
    auto position = modern_start_position();
    for (const char* move : {"h2h3", "b7c7", "e13e11", "m5l5",
                             "i1g3", "b9c9"}) {
        play_uci(position, move);
    }
    ASSERT_FALSE(position.in_check());

    chess::Move legal_moves[chess::MOVE_NB];
    const int legal_count = chess::generate_legal_moves(position, legal_moves);
    std::vector<std::uint32_t> expected;
    for (int index = 0; index < legal_count; ++index) {
        const auto move = legal_moves[index];
        if (position.board(move.target()) != chess::PieceColor::empty() ||
            move.policy() == chess::Move::Policy::Enpass ||
            move.policy() == chess::Move::Policy::Evolve) {
            expected.push_back(move.value());
        }
    }

    chess::Move noisy_moves[chess::MOVE_NB];
    const int noisy_count = chess::generate_noisy_moves(position, noisy_moves);
    std::vector<std::uint32_t> actual;
    for (int index = 0; index < noisy_count; ++index) {
        actual.push_back(noisy_moves[index].value());
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, expected);
}

TEST(PositionTest, EveryStartingMoveCanBeUndoneExactly) {
    auto position = modern_start_position();
    const auto original_fen = position.fen();
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    ASSERT_GT(count, 0);

    for (int i = 0; i < count; ++i) {
        position.make_move(moves[i]);
        EXPECT_EQ(position.key(), chess::zobrist::recompute(position))
            << moves[i].uci();
        position.undo_move(moves[i]);
        EXPECT_EQ(position.fen(), original_fen) << moves[i].uci();
        EXPECT_EQ(position.key(), chess::zobrist::recompute(position))
            << moves[i].uci();
    }
}

TEST(PositionTest, PromotionCanBeUndoneExactly) {
    auto position = modern_start_position();
    bool moved_pawn = false;
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = position.board(id);
        if (piece == chess::PieceColor(chess::Color::ID::Red,
                                      chess::Piece::ID::Pawn)) {
            position.pop_board(chess::Square(id), piece);
            position.set_board(chess::Square("e11"), piece);
            moved_pawn = true;
            break;
        }
    }
    ASSERT_TRUE(moved_pawn);
    position.init(position.fen());
    const auto original_fen = position.fen();

    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    bool tested = false;
    for (int i = 0; i < count; ++i) {
        if (moves[i].uci() == "e11e12Q") {
            position.make_move(moves[i]);
            EXPECT_EQ(position.key(), chess::zobrist::recompute(position));
            position.undo_move(moves[i]);
            EXPECT_EQ(position.fen(), original_fen);
            EXPECT_EQ(position.key(), chess::zobrist::recompute(position));
            tested = true;
            break;
        }
    }
    EXPECT_TRUE(tested);
}

TEST(PositionTest, ZobristHashTracksMovesAndUndo) {
    auto position = modern_start_position();
    const auto original_hash = chess::zobrist::hash(position);
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    ASSERT_GT(count, 0);

    position.make_move(moves[0]);
    EXPECT_NE(chess::zobrist::hash(position), original_hash);
    EXPECT_EQ(chess::zobrist::hash(position),
              chess::zobrist::recompute(position));
    position.undo_move(moves[0]);
    EXPECT_EQ(chess::zobrist::hash(position), original_hash);
    EXPECT_EQ(chess::zobrist::hash(position),
              chess::zobrist::recompute(position));
}

TEST(PositionTest, DetectsThreefoldRepetitionAcrossFourPlayerRounds) {
    auto position = modern_start_position();
    const std::array<std::string, 8> cycle = {
        "e1d3", "a5c4", "e14d12", "n5l4",
        "d3e1", "c4a5", "d12e14", "l4n5",
    };

    for (const auto& move : cycle) play_uci(position, move);
    EXPECT_FALSE(position.is_repetition());
    for (const auto& move : cycle) play_uci(position, move);
    EXPECT_TRUE(position.is_repetition());
    EXPECT_EQ(position.key(), chess::zobrist::recompute(position));
}

TEST(PositionTest, FourPlayerFiftyMoveClockUsesCompleteRounds) {
    auto position = modern_start_position();
    position.state().fifty_move_clock = 199;
    EXPECT_FALSE(position.is_fifty_move_draw());
    position.state().fifty_move_clock = 200;
    EXPECT_TRUE(position.is_fifty_move_draw());
    position.state().fifty_move_clock = 300;
    EXPECT_TRUE(position.is_fifty_move_draw());
}

TEST(PositionTest, IncrementalHashMatchesExhaustiveMoveTrees) {
    auto starting_position = modern_start_position();
    HashWalkStats starting_stats;
    verify_hash_tree(starting_position, 5, starting_stats);
    EXPECT_EQ(starting_stats.nodes, 3'612'576U);
    EXPECT_EQ(starting_stats.mismatches, 0U);
    EXPECT_EQ(starting_stats.en_passants, 1'580U);

    auto castling_position = castling_test_position();
    HashWalkStats castling_stats;
    verify_hash_tree(castling_position, 4, castling_stats);
    EXPECT_EQ(castling_stats.nodes, 343'252U);
    EXPECT_EQ(castling_stats.mismatches, 0U);
    EXPECT_EQ(castling_stats.castles, 1'150U);
}

TEST(PositionTest, FastAttackCheckAndEvaluationPathsMatchReferences) {
    FastPathStats stats;
    auto starting_position = modern_start_position();
    verify_fast_paths_tree(starting_position, 4, stats);

    auto castling_position = castling_test_position();
    verify_fast_paths_tree(castling_position, 1, stats);

    auto promotion_position = modern_start_position();
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = promotion_position.board(id);
        if (piece == chess::PieceColor(chess::Color::ID::Red,
                                      chess::Piece::ID::Pawn)) {
            promotion_position.pop_board(chess::Square(id), piece);
            promotion_position.set_board(chess::Square("e11"), piece);
            break;
        }
    }
    promotion_position.init(promotion_position.fen());
    verify_fast_paths_tree(promotion_position, 1, stats);

    EXPECT_GT(stats.nodes, 150'000U);
    EXPECT_GT(stats.moves, 150'000U);
    EXPECT_GT(stats.castles, 0U);
    EXPECT_GT(stats.promotions, 0U);
    EXPECT_EQ(stats.attack_mismatches, 0U);
    EXPECT_EQ(stats.check_mismatches, 0U);
    EXPECT_EQ(stats.evaluation_mismatches, 0U);
}

TEST(TranspositionTableTest, MainEntriesRespectCheckExtensionBudgetInQuiescence) {
    core::TranspositionTable::Entry entry;
    entry.quiescence = false;
    entry.extensions_used = 4;

    EXPECT_FALSE(entry.covers_quiescence(16, 0));
    EXPECT_FALSE(entry.covers_quiescence(16, 3));
    EXPECT_TRUE(entry.covers_quiescence(16, 4));

    entry.quiescence = true;
    entry.quiescence_depth = 7;
    EXPECT_FALSE(entry.covers_quiescence(8, 0));
    EXPECT_TRUE(entry.covers_quiescence(7, 0));
}

TEST(TranspositionTableTest, SameCapacityResizePreservesEntries) {
    core::TranspositionTable table(16);
    constexpr chess::zobrist::Key key = 0x123456789abcdef0ULL;
    table.store(key, 5, 0, 42, core::TranspositionTable::Bound::Exact,
                chess::Move{});
    ASSERT_NE(table.probe(key), nullptr);

    table.resize(16);

    const auto* entry = table.probe(key);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->score, 42);
    EXPECT_EQ(entry->depth, 5);
}

TEST(TranspositionTableTest, ClusterRetainsFourCollidingEntries) {
    core::TranspositionTable table(1);
    constexpr chess::zobrist::Key base = 0x100;
    for (std::uint64_t offset = 0; offset < 4; ++offset) {
        table.store(base + offset, static_cast<int>(offset + 1), 0,
                    static_cast<core::Score>(10 + offset),
                    core::TranspositionTable::Bound::Exact, chess::Move{});
    }
    for (std::uint64_t offset = 0; offset < 4; ++offset) {
        ASSERT_NE(table.probe(base + offset), nullptr) << offset;
    }

    const auto replacement_key = base + table.size();
    table.store(replacement_key, 10, 0, 99,
                core::TranspositionTable::Bound::Exact, chess::Move{});
    EXPECT_EQ(table.probe(base), nullptr);
    EXPECT_NE(table.probe(replacement_key), nullptr);
    for (std::uint64_t offset = 1; offset < 4; ++offset) {
        EXPECT_NE(table.probe(base + offset), nullptr) << offset;
    }
}

TEST(PositionTest, DetectsCheckForAPlayerWhoIsNotOnMove) {
    auto position = coordinated_mate_position();
    position.set_board(chess::Square("k8"),
        chess::PieceColor(chess::Color::ID::Yellow, chess::Piece::ID::Rook));
    position.init(position.fen());

    EXPECT_FALSE(position.in_check());
    EXPECT_TRUE(position.in_check(chess::Color::ID::Green));
}

TEST(PositionTest, EnPassantOnlyUsesTheApproachBesideTheStridingPawn) {
    auto position = modern_start_position();
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        const auto piece = position.board(id);
        if (piece != chess::PieceColor::empty() &&
            piece != chess::PieceColor::stone()) {
            position.pop_board(chess::Square(id), piece);
        }
    }

    const auto place = [&position](const char* square, chess::Color::ID color,
                                   chess::Piece::ID piece) {
        position.set_board(chess::Square(square), chess::PieceColor(color, piece));
    };
    place("h2", chess::Color::ID::Red, chess::Piece::ID::King);
    place("b8", chess::Color::ID::Blue, chess::Piece::ID::King);
    place("h14", chess::Color::ID::Yellow, chess::Piece::ID::King);
    place("n8", chess::Color::ID::Green, chess::Piece::ID::King);
    place("k3", chess::Color::ID::Red, chess::Piece::ID::Pawn);
    place("m3", chess::Color::ID::Red, chess::Piece::ID::Pawn);
    place("m4", chess::Color::ID::Green, chess::Piece::ID::Pawn);
    position.init(position.fen());

    const auto normal_move = [](const char* source, const char* target,
                                chess::Move::Policy policy) {
        return chess::Move(chess::Square(source).id(), chess::Square(target).id(),
            chess::Piece::ID::Empty, chess::Color::ID::None,
            chess::Castle::Side::KingSide, policy);
    };
    position.make_move(normal_move("h2", "g2", chess::Move::Policy::Normal));
    position.make_move(normal_move("b8", "b7", chess::Move::Policy::Normal));
    position.make_move(normal_move("h14", "g14", chess::Move::Policy::Normal));
    position.make_move(normal_move("m4", "k4", chess::Move::Policy::Stride));

    const auto original_fen = position.fen();
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    int en_passant_count = 0;
    for (int i = 0; i < count; ++i) {
        if (moves[i].policy() != chess::Move::Policy::Enpass) continue;
        ++en_passant_count;
        EXPECT_EQ(moves[i].uci(), "k3l4");
        const auto mover = position.turn();
        const auto next = mover.next().id();
        const auto previous = mover.prev().id();
        const bool predicts_next = position.would_check(moves[i], next);
        const bool predicts_previous = position.would_check(moves[i], previous);
        position.make_move(moves[i]);
        EXPECT_TRUE(position.consistent());
        EXPECT_EQ(position.key(), chess::zobrist::recompute(position));
        EXPECT_EQ(predicts_next, position.get_attackers_bitboard(
            position.royal(next), chess::Color(next),
            position.occupied()).any());
        EXPECT_EQ(predicts_previous, position.get_attackers_bitboard(
            position.royal(previous), chess::Color(previous),
            position.occupied()).any());
        EXPECT_EQ(core::evaluate(position),
            core::evaluate_material(position) +
            core::evaluate_positional(position));
        position.undo_move(moves[i]);
        EXPECT_TRUE(position.consistent());
        EXPECT_EQ(position.fen(), original_fen);
        EXPECT_EQ(position.key(), chess::zobrist::recompute(position));
    }
    EXPECT_EQ(en_passant_count, 1);
}

TEST(PositionTest, CastlingRequiresKingAndRookOnTheirSourceSquares) {
    auto position = coordinated_mate_position();
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    for (int i = 0; i < count; ++i) {
        EXPECT_NE(moves[i].policy(), chess::Move::Policy::Castle);
    }
}

TEST(MoveTest, PromotionUsesTheUciPieceLetter) {
    const chess::Move promotion(
        chess::Square("e10").id(), chess::Square("e11").id(),
        chess::Piece::ID::Queen, chess::Color::ID::None,
        static_cast<chess::Castle::Side>(2), chess::Move::Policy::Evolve);
    EXPECT_EQ(promotion.uci(), "e10e11Q");
}

TEST(SearchTest, ReturnsALegalMoveAtRequestedDepth) {
    auto position = modern_start_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 2;
    search.reset();

    const auto result = search.think(position, limits);

    EXPECT_FALSE(result.best_move.is_null());
    EXPECT_EQ(result.depth, 2);
    EXPECT_GT(result.nodes, 0U);
    EXPECT_GT(result.quiescence_nodes, 0U);
    ASSERT_GE(result.principal_variation.size(), 2U);

    chess::Move legal_moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, legal_moves);
    bool found = false;
    for (int i = 0; i < count; ++i) {
        if (legal_moves[i].uci() == result.best_move.uci()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);

    auto line = position;
    for (const auto move : result.principal_variation) {
        chess::Move line_moves[chess::MOVE_NB];
        const int line_count = chess::generate_legal_moves(line, line_moves);
        bool line_move_is_legal = false;
        for (int i = 0; i < line_count; ++i) {
            if (line_moves[i] == move) {
                line_move_is_legal = true;
                break;
            }
        }
        ASSERT_TRUE(line_move_is_legal) << move.uci();
        line.make_move(move);
    }
}

TEST(SearchTest, RanksLegalRootMovesForGuiAnalysis) {
    auto position = modern_start_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 2;
    search.reset();

    std::vector<int> completed_depths;
    const auto result = search.analyze(position, limits, 5,
        [&](const core::Search::AnalysisResult& update) {
            if (update.iteration_complete) {
                completed_depths.push_back(update.depth);
            }
        });

    ASSERT_EQ(result.lines.size(), 5U);
    EXPECT_GT(result.nodes, 0U);
    EXPECT_EQ(result.depth, 2);
    EXPECT_TRUE(result.iteration_complete);
    EXPECT_EQ(result.root_moves_completed, result.root_move_count);
    ASSERT_GE(completed_depths.size(), 2U);
    EXPECT_EQ(completed_depths[completed_depths.size() - 2], 1);
    EXPECT_EQ(completed_depths.back(), 2);
    chess::Move legal_moves[chess::MOVE_NB];
    const int legal_count = chess::generate_legal_moves(position, legal_moves);
    for (const auto& line : result.lines) {
        EXPECT_FALSE(line.move.is_null());
        ASSERT_FALSE(line.principal_variation.empty());
        EXPECT_EQ(line.principal_variation.front(), line.move);
        bool found = false;
        for (int index = 0; index < legal_count; ++index) {
            if (line.move == legal_moves[index]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << line.move.uci();
    }
}

TEST(SearchTest, ParallelRankedAnalysisMatchesSingleThreadedScores) {
    const auto position = modern_start_position();
    core::Search::Limits limits;
    limits.depth = 2;

    core::Search single_threaded;
    single_threaded.set_threads(1);
    single_threaded.reset();
    const auto expected = single_threaded.analyze(position, limits, 64);

    core::Search parallel;
    parallel.set_threads(4);
    parallel.reset();
    const auto actual = parallel.analyze(position, limits, 64);

    ASSERT_EQ(actual.lines.size(), expected.lines.size());
    EXPECT_TRUE(actual.iteration_complete);
    EXPECT_EQ(actual.root_moves_completed, actual.root_move_count);
    EXPECT_GT(actual.nodes, 0U);
    for (const auto& expected_line : expected.lines) {
        const auto found = std::find_if(actual.lines.begin(), actual.lines.end(),
            [&](const core::Search::AnalysisLine& line) {
                return line.move == expected_line.move;
            });
        ASSERT_NE(found, actual.lines.end()) << expected_line.move.uci();
        EXPECT_EQ(found->score, expected_line.score)
            << expected_line.move.uci();
    }
}

TEST(SearchTest, WarmRankedAnalysisReconstructsPrincipalVariations) {
    const auto position = modern_start_position();
    core::Search::Limits limits;
    limits.depth = 3;

    core::Search search;
    search.set_threads(1);
    search.clear_hash();
    const auto first = search.analyze(position, limits, 64);
    const auto second = search.analyze(position, limits, 64);

    EXPECT_TRUE(first.iteration_complete);
    EXPECT_TRUE(second.iteration_complete);
    EXPECT_LT(second.nodes, first.nodes);
    EXPECT_GT(second.tt_hits, 0U);
    ASSERT_FALSE(second.lines.empty());
    EXPECT_GE(second.lines.front().principal_variation.size(), 3U);
}

TEST(SearchTest, ThreadCountIsClampedToSupportedRange) {
    core::Search search;
    search.set_threads(0);
    EXPECT_EQ(search.threads(), 1);
    search.set_threads(8);
    EXPECT_EQ(search.threads(), 4);
}

TEST(SearchTest, RankedAnalysisSearchesForcingRootMovesFirst) {
    const auto position = coordinated_mate_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 1;
    search.reset();
    std::string first_searched;

    search.analyze(position, limits, 5,
        [&](const core::Search::AnalysisResult& update) {
            if (update.depth != 1 || update.root_moves_completed != 1) return;
            for (const auto& line : update.lines) {
                if (line.depth == 1) {
                    first_searched = line.move.uci();
                    break;
                }
            }
        });

    EXPECT_EQ(first_searched, "b4b7");
}

TEST(SearchTest, InfiniteRankedAnalysisCanBeStopped) {
    const auto position = modern_start_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 64;
    limits.infinite = true;
    search.reset();

    auto future = std::async(std::launch::async, [&]() {
        return search.analyze(position, limits, 5);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    search.stop();

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto result = future.get();
    EXPECT_FALSE(result.lines.empty());
    EXPECT_GT(result.nodes, 0U);
}

TEST(SearchTest, FindsAForcingCheckmateDespiteTheRookSacrifice) {
    auto position = coordinated_mate_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 1;
    search.reset();

    const auto result = search.think(position, limits);

    EXPECT_TRUE(core::is_mate_score(result.score));
    EXPECT_GT(result.score, 0);
    EXPECT_EQ(result.best_move.uci(), "b4b7");
    EXPECT_GT(result.check_extensions, 0U);
    ASSERT_FALSE(result.principal_variation.empty());

    position.make_move(result.best_move);
    chess::Move replies[chess::MOVE_NB];
    EXPECT_EQ(chess::generate_legal_moves(position, replies), 0);
    EXPECT_TRUE(position.in_check());
}

TEST(SearchTest, QuiescenceIncludesQuietForcingChecks) {
    auto position = quiet_check_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 1;
    search.reset();

    const auto result = search.think(position, limits);

    EXPECT_GT(result.quiescence_checks, 0U);
    EXPECT_GT(result.selective_depth, result.depth);
}

TEST(SearchTest, StartingPositionDoesNotProduceAFalseMateAtDepthSix) {
    auto position = modern_start_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 6;
    search.reset();

    const auto result = search.think(position, limits);

    EXPECT_FALSE(core::is_mate_score(result.score));
    ASSERT_FALSE(result.best_move.is_null());
    ASSERT_GE(result.principal_variation.size(), 6U);
    for (const auto move : result.principal_variation) {
        chess::Move legal_moves[chess::MOVE_NB];
        const int count = chess::generate_legal_moves(position, legal_moves);
        bool found = false;
        for (int i = 0; i < count; ++i) {
            if (legal_moves[i] == move) {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found) << move.uci();
        position.make_move(move);
    }
}

TEST(SearchTest, ScoresThreefoldRepetitionAsADraw) {
    auto position = modern_start_position();
    const std::array<std::string, 8> cycle = {
        "e1d3", "a5c4", "e14d12", "n5l4",
        "d3e1", "c4a5", "d12e14", "l4n5",
    };
    for (int repetition = 0; repetition < 2; ++repetition) {
        for (const auto& move : cycle) play_uci(position, move);
    }
    ASSERT_TRUE(position.is_repetition());

    core::Search search;
    core::Search::Limits limits;
    limits.depth = 2;
    const auto result = search.think(position, limits);

    EXPECT_EQ(result.score, core::SCORE_DRAW);
}

TEST(SearchTest, ScoresFiftyMovePositionAsADraw) {
    auto position = modern_start_position();
    const auto blue_queen = chess::PieceColor(
        chess::Color::ID::Blue, chess::Piece::ID::Queen);
    for (int square = 0; square < chess::SQUARE_NB; ++square) {
        const auto id = static_cast<chess::Square::ID>(square);
        if (position.board(id) == blue_queen) {
            position.pop_board(chess::Square(id), blue_queen);
            break;
        }
    }
    ASSERT_GT(core::evaluate(position), core::SCORE_DRAW);
    position.state().fifty_move_clock = 200;

    core::Search search;
    core::Search::Limits limits;
    limits.depth = 2;
    const auto result = search.think(position, limits);

    EXPECT_EQ(result.score, core::SCORE_DRAW);
}

TEST(SearchTest, ReusesTranspositionTableResults) {
    const auto position = modern_start_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 3;
    search.clear_hash();
    search.reset();
    const auto first = search.think(position, limits);

    search.reset();
    const auto second = search.think(position, limits);

    EXPECT_GT(second.tt_hits, 0U);
    EXPECT_LT(second.nodes, first.nodes);
    EXPECT_EQ(second.best_move, first.best_move);
    EXPECT_EQ(second.score, first.score);
}

TEST(SearchTest, InfiniteSearchCanBeStopped) {
    const auto position = modern_start_position();
    core::Search search;
    core::Search::Limits limits;
    limits.depth = 64;
    limits.infinite = true;
    search.reset();

    auto future = std::async(std::launch::async, [&]() {
        return search.think(position, limits);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    search.stop();

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_FALSE(future.get().best_move.is_null());
}

} // namespace
