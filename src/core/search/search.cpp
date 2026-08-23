#include "search.h"
#include <algorithm>
#include <array>
#include <iostream>
#include "chess/constants.h"
#include "chess/movegen.h"
#include "chess/zobrist.h"

namespace athena::core {

namespace {

constexpr std::array<int, chess::PIECE_NB> ORDER_VALUE = {
    20'000, 300, 350, 500, 1'000, 100
};

constexpr int MAX_CHECK_EXTENSIONS = 4;
constexpr int MAX_QUIESCENCE_PLY = 16;
constexpr int MAX_QUIESCENCE_CHECK_PLY = 6;

enum CheckTarget : std::uint8_t {
    NoCheck = 0,
    ImmediateOpponent = 1,
    OtherOpponent = 2,
};

bool is_noisy(const chess::Position& position, chess::Move move) noexcept {
    return position.board(move.target()) != chess::PieceColor::empty() ||
        move.policy() == chess::Move::Policy::Enpass ||
        move.policy() == chess::Move::Policy::Evolve;
}

std::uint8_t check_targets_after_move(chess::Position& position,
                                      chess::Move move) noexcept {
    const auto mover = position.turn();
    position.make_move(move);
    std::uint8_t targets = NoCheck;
    if (position.in_check()) targets |= ImmediateOpponent;
    if (position.in_check(mover.prev().id())) targets |= OtherOpponent;
    position.undo_move(move);
    return targets;
}

int move_order_score(const chess::Position& position, chess::Move move,
                     chess::Move table_move, std::uint8_t checks) noexcept {
    if (!table_move.is_null() && move == table_move) return 1'000'000;
    int score = 0;
    const auto captured = position.board(move.target());
    if (captured != chess::PieceColor::empty()) {
        const auto attacker = position.board(move.source());
        score += 20'000
            + ORDER_VALUE[static_cast<std::size_t>(captured.piece().id())]
            - ORDER_VALUE[static_cast<std::size_t>(attacker.piece().id())] / 10;
    }
    if (move.policy() == chess::Move::Policy::Enpass) score += 20'000;
    if (move.policy() == chess::Move::Policy::Evolve) {
        score += 25'000 + ORDER_VALUE[static_cast<std::size_t>(move.evolve())];
    }
    if ((checks & ImmediateOpponent) != 0) score += 40'000;
    if ((checks & OtherOpponent) != 0) score += 30'000;
    return score;
}

void order_moves(chess::Position& position, chess::Move* moves, int move_count,
                 chess::Move table_move) {
    struct ScoredMove {
        chess::Move move;
        int score;
    };

    std::array<ScoredMove, chess::MOVE_NB> scored{};
    for (int i = 0; i < move_count; ++i) {
        const auto checks = check_targets_after_move(position, moves[i]);
        scored[static_cast<std::size_t>(i)] = {
            moves[i], move_order_score(position, moves[i], table_move, checks)};
    }

    std::stable_sort(scored.begin(), scored.begin() + move_count,
        [](const ScoredMove& lhs, const ScoredMove& rhs) {
            return lhs.score > rhs.score;
        });
    for (int i = 0; i < move_count; ++i) {
        moves[i] = scored[static_cast<std::size_t>(i)].move;
    }
}

Score score_to_table(Score score, int ply) noexcept {
    if (!is_mate_score(score)) return score;
    return score > 0 ? score + ply : score - ply;
}

Score score_from_table(Score score, int ply) noexcept {
    if (!is_mate_score(score)) return score;
    return score > 0 ? score - ply : score + ply;
}

} // namespace

void Search::reset() noexcept {
    stop_requested_.store(false, std::memory_order_relaxed);
}

void Search::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

bool Search::should_stop() noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (heartbeat_ && now >= next_heartbeat_) {
        next_heartbeat_ = now + std::chrono::milliseconds(200);
        try {
            heartbeat_();
        } catch (...) {
            stop_requested_.store(true, std::memory_order_relaxed);
        }
    }
    if (stop_requested_.load(std::memory_order_relaxed)) return true;
    return has_deadline_ && now >= deadline_;
}

Search::Result Search::think(const chess::Position& position, const Limits& limits) {
    Result result;
    chess::Position current = position;
    const auto started = std::chrono::steady_clock::now();

    nodes_ = 0;
    quiescence_nodes_ = 0;
    tt_hits_ = 0;
    check_extensions_ = 0;
    quiescence_checks_ = 0;
    selective_depth_ = 0;
    aborted_ = false;
    has_deadline_ = limits.move_time.count() > 0 && !limits.infinite;
    if (has_deadline_) deadline_ = started + limits.move_time;

    const int remaining_history =
        chess::PLAY_NB - static_cast<int>(position.play()) - 1;
    const int maximum_depth = std::clamp(limits.depth, 1,
        std::max(1, std::min(64, remaining_history)));

    for (int depth = 1; depth <= maximum_depth; ++depth) {
        chess::Move iteration_best;
        aborted_ = false;
        const Score score = alpha_beta(current, depth, -SCORE_INFINITE,
                                       SCORE_INFINITE, 0, 0, &iteration_best);
        if (aborted_) break;

        result.best_move = iteration_best;
        result.score = score;
        result.depth = depth;
        result.selective_depth = selective_depth_;
        result.nodes = nodes_;
        result.quiescence_nodes = quiescence_nodes_;
        result.tt_hits = tt_hits_;
        result.check_extensions = check_extensions_;
        result.quiescence_checks = quiescence_checks_;
        result.principal_variation.clear();
        const int pv_length = pv_length_[0];
        for (int index = 0; index < pv_length; ++index) {
            result.principal_variation.push_back(
                pv_table_[0][static_cast<std::size_t>(index)]);
        }
        if (result.principal_variation.empty()) {
            result.principal_variation = extract_principal_variation(
                current, depth + MAX_CHECK_EXTENSIONS);
        }
        if (result.principal_variation.empty() && !iteration_best.is_null()) {
            result.principal_variation.push_back(iteration_best);
        }
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        const auto elapsed_ms = std::max<std::int64_t>(1, result.elapsed.count());
        const auto nps = result.nodes * 1000ULL /
            static_cast<std::uint64_t>(elapsed_ms);

        std::cout << "info depth " << result.depth
                  << " seldepth " << result.selective_depth
                  << " score ";
        if (is_mate_score(result.score)) {
            std::cout << "mate " << mate_in(result.score);
        } else {
            std::cout << "cp " << result.score;
        }
        std::cout << " nodes " << result.nodes
                  << " qnodes " << result.quiescence_nodes
                  << " tthits " << result.tt_hits
                  << " checkext " << result.check_extensions
                  << " qchecks " << result.quiescence_checks
                  << " nps " << nps
                  << " time " << result.elapsed.count();
        if (!result.principal_variation.empty()) {
            std::cout << " pv";
            for (const auto move : result.principal_variation) {
                std::cout << ' ' << move.uci();
            }
        }
        std::cout << std::endl;

        if (is_mate_score(score) || should_stop()) break;
    }

    result.nodes = nodes_;
    result.quiescence_nodes = quiescence_nodes_;
    result.tt_hits = tt_hits_;
    result.check_extensions = check_extensions_;
    result.quiescence_checks = quiescence_checks_;
    result.selective_depth = selective_depth_;
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // A very short movetime can expire before depth one completes. Returning
    // the first legal move is preferable to emitting a null move in that case.
    if (result.best_move.is_null()) {
        chess::Move moves[chess::MOVE_NB];
        const int count = chess::generate_legal_moves(current, moves);
        if (count > 0) result.best_move = moves[0];
    }

    std::cout << "bestmove " << result.best_move.uci() << std::endl;
    return result;
}

Search::AnalysisResult Search::analyze(const chess::Position& position,
                                       const Limits& limits,
                                       std::size_t max_lines,
                                       const AnalysisCallback& callback) {
    AnalysisResult result;
    chess::Position current = position;
    const auto started = std::chrono::steady_clock::now();

    nodes_ = 0;
    quiescence_nodes_ = 0;
    tt_hits_ = 0;
    check_extensions_ = 0;
    quiescence_checks_ = 0;
    selective_depth_ = 0;
    aborted_ = false;
    has_deadline_ = limits.move_time.count() > 0 && !limits.infinite;
    if (has_deadline_) deadline_ = started + limits.move_time;

    const int remaining_history =
        chess::PLAY_NB - static_cast<int>(position.play()) - 1;
    const int maximum_depth = std::clamp(limits.depth, 1,
        std::max(1, std::min(64, remaining_history)));

    chess::Move moves[chess::MOVE_NB];
    const int move_count = chess::generate_legal_moves(current, moves);
    order_moves(current, moves, move_count, chess::Move{});

    std::vector<AnalysisLine> previous_lines;
    previous_lines.reserve(static_cast<std::size_t>(move_count));
    for (int index = 0; index < move_count; ++index) {
        const auto move = moves[index];
        current.make_move(move);
        const Score score = -evaluate(current);
        current.undo_move(move);
        previous_lines.push_back({move, score, 0, {move}});
    }

    auto make_snapshot = [&](const std::vector<AnalysisLine>& current_lines,
                             int depth, int completed,
                             bool iteration_complete) {
        AnalysisResult snapshot;
        snapshot.lines = current_lines;
        for (const auto& previous : previous_lines) {
            const bool replaced = std::any_of(snapshot.lines.begin(),
                snapshot.lines.end(), [&](const AnalysisLine& line) {
                    return line.move == previous.move;
                });
            if (!replaced) snapshot.lines.push_back(previous);
        }
        std::stable_sort(snapshot.lines.begin(), snapshot.lines.end(),
            [](const AnalysisLine& lhs, const AnalysisLine& rhs) {
                return lhs.score > rhs.score;
            });
        if (max_lines > 0 && snapshot.lines.size() > max_lines) {
            snapshot.lines.resize(max_lines);
        }
        snapshot.depth = depth;
        snapshot.root_moves_completed = completed;
        snapshot.root_move_count = move_count;
        snapshot.iteration_complete = iteration_complete;
        snapshot.selective_depth = selective_depth_;
        snapshot.nodes = nodes_;
        snapshot.quiescence_nodes = quiescence_nodes_;
        snapshot.tt_hits = tt_hits_;
        snapshot.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return snapshot;
    };

    std::vector<AnalysisLine> iteration_lines;
    result = make_snapshot(iteration_lines, 0, 0, move_count == 0);
    if (callback) callback(result);

    for (int depth = 1; depth <= maximum_depth && move_count > 0; ++depth) {
        iteration_lines.clear();
        iteration_lines.reserve(static_cast<std::size_t>(move_count));
        int completed = 0;

        auto publish = [&]() {
            result = make_snapshot(iteration_lines, depth, completed,
                                   completed == move_count);
            if (callback) callback(result);
        };
        heartbeat_ = publish;
        next_heartbeat_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(200);

        while (completed < move_count) {
            if (should_stop()) break;

            pv_length_.fill(0);
            aborted_ = false;
            const auto move = moves[completed];
            current.make_move(move);
            const Score score = -alpha_beta(current, depth - 1,
                -SCORE_INFINITE, SCORE_INFINITE, 1, 0, nullptr);
            current.undo_move(move);
            if (aborted_) break;

            AnalysisLine line;
            line.move = move;
            line.score = score;
            line.depth = depth;
            line.principal_variation.push_back(move);
            const int child_length = pv_length_[1];
            for (int index = 1; index < child_length; ++index) {
                line.principal_variation.push_back(
                    pv_table_[1][static_cast<std::size_t>(index)]);
            }
            iteration_lines.push_back(std::move(line));
            ++completed;
            publish();
        }

        if (completed == move_count) {
            previous_lines = iteration_lines;
            result = make_snapshot(iteration_lines, depth, completed, true);
        } else {
            result = make_snapshot(iteration_lines, depth, completed, false);
            if (callback) callback(result);
            break;
        }
        if (should_stop()) break;
    }

    heartbeat_ = {};
    return result;
}

Score Search::alpha_beta(chess::Position& position, int depth, Score alpha,
                         Score beta, int ply, int extensions_used,
                         chess::Move* root_best) {
    if (ply >= MAX_PV_PLY - 1) return evaluate(position);
    pv_length_[static_cast<std::size_t>(ply)] = ply;

    if (position.in_check() && extensions_used < MAX_CHECK_EXTENSIONS) {
        ++depth;
        ++extensions_used;
        ++check_extensions_;
    }

    if (depth <= 0) {
        return quiescence(position, alpha, beta, ply, 0);
    }

    ++nodes_;
    selective_depth_ = std::max(selective_depth_, ply);

    if ((nodes_ & 1023ULL) == 0 && should_stop()) {
        aborted_ = true;
        return SCORE_DRAW;
    }

    const auto key = chess::zobrist::hash(position);
    const Score original_alpha = alpha;
    chess::Move table_move;
    if (const auto* entry = table_.probe(key)) {
        ++tt_hits_;
        table_move = entry->best_move;
        // Check extensions are path-dependent. Reuse a cutoff only when the
        // stored search had at least as much extension budget available.
        if (entry->depth >= depth &&
            entry->extensions_used <= extensions_used) {
            const Score table_score = score_from_table(entry->score, ply);
            const bool cutoff =
                entry->bound == TranspositionTable::Bound::Exact ||
                (entry->bound == TranspositionTable::Bound::Lower && table_score >= beta) ||
                (entry->bound == TranspositionTable::Bound::Upper && table_score <= alpha);
            if (cutoff) {
                if (root_best != nullptr) *root_best = entry->best_move;
                return table_score;
            }
        }
    }

    chess::Move moves[chess::MOVE_NB];
    const int move_count = chess::generate_legal_moves(position, moves);
    if (move_count == 0) {
        const Score terminal = position.in_check()
            ? -SCORE_MATE + ply
            : SCORE_DRAW;
        table_.store(key, depth, extensions_used, score_to_table(terminal, ply),
                     TranspositionTable::Bound::Exact, chess::Move{});
        return terminal;
    }

    order_moves(position, moves, move_count, table_move);

    Score best = -SCORE_INFINITE;
    chess::Move best_move;
    for (int i = 0; i < move_count; ++i) {
        const auto move = moves[i];
        position.make_move(move);
        const Score score = -alpha_beta(position, depth - 1, -beta, -alpha,
                                        ply + 1, extensions_used, nullptr);
        position.undo_move(move);

        if (aborted_) return SCORE_DRAW;

        if (score > best) {
            best = score;
            best_move = move;
            update_principal_variation(ply, move);
            if (root_best != nullptr) *root_best = move;
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) break;
    }

    auto bound = TranspositionTable::Bound::Exact;
    if (best <= original_alpha) bound = TranspositionTable::Bound::Upper;
    if (best >= beta) bound = TranspositionTable::Bound::Lower;
    table_.store(key, depth, extensions_used, score_to_table(best, ply), bound,
                 best_move);

    return best;
}

Score Search::quiescence(chess::Position& position, Score alpha, Score beta,
                         int ply, int quiescence_ply) {
    if (ply >= MAX_PV_PLY - 1) return evaluate(position);
    pv_length_[static_cast<std::size_t>(ply)] = ply;

    ++nodes_;
    ++quiescence_nodes_;
    selective_depth_ = std::max(selective_depth_, ply);

    if ((nodes_ & 1023ULL) == 0 && should_stop()) {
        aborted_ = true;
        return SCORE_DRAW;
    }

    if (position.play() >= chess::PLAY_NB - 1) return evaluate(position);

    const bool in_check = position.in_check();
    if (quiescence_ply >= MAX_QUIESCENCE_PLY) {
        return evaluate(position) - (in_check ? 50 : 0);
    }
    chess::Move moves[chess::MOVE_NB];
    int move_count = 0;

    if (in_check) {
        move_count = chess::generate_legal_moves(position, moves);
        if (move_count == 0) return -SCORE_MATE + ply;
    } else {
        const Score stand_pat = evaluate(position);
        if (stand_pat >= beta) return stand_pat;
        alpha = std::max(alpha, stand_pat);
        move_count = chess::generate_legal_moves(position, moves);
        int forcing_count = 0;
        for (int i = 0; i < move_count; ++i) {
            const auto move = moves[i];
            bool include = is_noisy(position, move);
            if (!include && quiescence_ply < MAX_QUIESCENCE_CHECK_PLY) {
                const auto checks = check_targets_after_move(position, move);
                include = (checks & ImmediateOpponent) != 0;
                if (include) ++quiescence_checks_;
            }
            if (include) moves[forcing_count++] = move;
        }
        move_count = forcing_count;
        if (move_count == 0) return stand_pat;
    }

    order_moves(position, moves, move_count, chess::Move{});

    for (int i = 0; i < move_count; ++i) {
        const auto move = moves[i];
        position.make_move(move);
        const Score score = -quiescence(position, -beta, -alpha, ply + 1,
                                        quiescence_ply + 1);
        position.undo_move(move);

        if (aborted_) return SCORE_DRAW;
        if (score >= beta) {
            update_principal_variation(ply, move);
            return score;
        }
        if (score > alpha) {
            alpha = score;
            update_principal_variation(ply, move);
        }
    }

    return alpha;
}

void Search::update_principal_variation(int ply, chess::Move move) noexcept {
    const auto row = static_cast<std::size_t>(ply);
    pv_table_[row][row] = move;
    const int child_length = pv_length_[static_cast<std::size_t>(ply + 1)];
    for (int index = ply + 1; index < child_length; ++index) {
        pv_table_[row][static_cast<std::size_t>(index)] =
            pv_table_[static_cast<std::size_t>(ply + 1)]
                     [static_cast<std::size_t>(index)];
    }
    pv_length_[row] = std::max(ply + 1, child_length);
}

std::vector<chess::Move> Search::extract_principal_variation(
    const chess::Position& position, int max_plies) const {
    std::vector<chess::Move> variation;
    chess::Position current = position;
    variation.reserve(static_cast<std::size_t>(max_plies));

    for (int ply = 0; ply < max_plies; ++ply) {
        const auto* entry = table_.probe(chess::zobrist::hash(current));
        if (entry == nullptr || entry->best_move.is_null()) break;

        chess::Move legal_moves[chess::MOVE_NB];
        const int move_count = chess::generate_legal_moves(current, legal_moves);
        chess::Move selected;
        for (int i = 0; i < move_count; ++i) {
            if (legal_moves[i] == entry->best_move) {
                selected = legal_moves[i];
                break;
            }
        }
        if (selected.is_null()) break;

        variation.push_back(selected);
        current.make_move(selected);
    }
    return variation;
}

} // namespace athena
