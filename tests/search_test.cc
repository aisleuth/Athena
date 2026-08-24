#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <future>
#include <string>
#include <thread>
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
        position.make_move(moves[i]);
        EXPECT_TRUE(position.consistent());
        EXPECT_EQ(position.key(), chess::zobrist::recompute(position));
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
