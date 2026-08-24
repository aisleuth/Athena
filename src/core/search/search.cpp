#include "search.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include "chess/constants.h"
#include "chess/movegen.h"
#include "chess/zobrist.h"

namespace athena::core {

namespace {

constexpr std::array<int, chess::PIECE_NB> ORDER_VALUE = {
    20'000, 300, 350, 500, 1'000, 100
};

constexpr int MAX_CHECK_EXTENSIONS = 2;
constexpr int MAX_QUIESCENCE_PLY = 16;
constexpr int MAX_QUIESCENCE_CHECK_PLY = 2;
constexpr Score DELTA_MARGIN = 150;

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

Score tactical_gain(const chess::Position& position,
                    chess::Move move) noexcept {
    Score gain = 0;
    const auto captured = position.board(move.target());
    if (captured != chess::PieceColor::empty()) {
        gain += ORDER_VALUE[static_cast<std::size_t>(captured.piece().id())];
    }
    if (move.policy() == chess::Move::Policy::Enpass ||
        (move.policy() == chess::Move::Policy::Evolve &&
         move.enpass() != chess::Color::ID::None)) {
        gain += ORDER_VALUE[static_cast<std::size_t>(chess::Piece::ID::Pawn)];
    }
    if (move.policy() == chess::Move::Policy::Evolve) {
        gain += ORDER_VALUE[static_cast<std::size_t>(move.evolve())]
            - ORDER_VALUE[static_cast<std::size_t>(chess::Piece::ID::Pawn)];
    }
    return gain;
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
                     chess::Move table_move, std::uint8_t checks,
                     const std::array<chess::Move, 2>* killers,
                     const std::int32_t* history) noexcept {
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
    if (!is_noisy(position, move)) {
        if (killers && move == (*killers)[0]) score += 15'000;
        else if (killers && move == (*killers)[1]) score += 14'000;
        if (history) {
            const auto index = static_cast<std::size_t>(move.source()) *
                chess::SQUARE_NB + static_cast<std::size_t>(move.target());
            score += std::min<std::int32_t>(10'000, history[index]);
        }
    }
    return score;
}

void order_moves(chess::Position& position, chess::Move* moves, int move_count,
                 chess::Move table_move, bool detect_checks = true,
                 const std::array<chess::Move, 2>* killers = nullptr,
                 const std::int32_t* history = nullptr) {
    struct ScoredMove {
        chess::Move move;
        int score;
    };

    std::array<ScoredMove, chess::MOVE_NB> scored{};
    for (int i = 0; i < move_count; ++i) {
        const std::uint8_t checks = detect_checks
            ? check_targets_after_move(position, moves[i])
            : static_cast<std::uint8_t>(NoCheck);
        scored[static_cast<std::size_t>(i)] = {
            moves[i], move_order_score(position, moves[i], table_move, checks,
                                       killers, history)};
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

Search::Search(std::size_t hash_megabytes)
    : table_(std::max<std::size_t>(1, hash_megabytes)),
      hash_megabytes_(std::max<std::size_t>(1, hash_megabytes)) {}

void Search::reset() noexcept {
    stop_requested_.store(false, std::memory_order_relaxed);
}

void Search::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

void Search::set_threads(int threads) noexcept {
    threads_ = std::clamp(threads, 1, 4);
}

void Search::prepare_worker(
    const Limits& limits, std::chrono::steady_clock::time_point started,
    const std::atomic_bool* external_stop) noexcept {
    reset();
    external_stop_ = external_stop;
    nodes_ = 0;
    quiescence_nodes_ = 0;
    tt_hits_ = 0;
    check_extensions_ = 0;
    quiescence_checks_ = 0;
    selective_depth_ = 0;
    aborted_ = false;
    has_deadline_ = limits.move_time.count() > 0 && !limits.infinite;
    if (has_deadline_) deadline_ = started + limits.move_time;
    heartbeat_ = {};
    table_.new_search();
    killers_ = {};
    history_ = {};
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
    if (stop_requested_.load(std::memory_order_relaxed) ||
        (external_stop_ && external_stop_->load(std::memory_order_relaxed))) {
        return true;
    }
    return has_deadline_ && now >= deadline_;
}

Search::Result Search::think(const chess::Position& position, const Limits& limits) {
    Result result;
    chess::Position current = position;
    const auto started = std::chrono::steady_clock::now();
    table_.resize(hash_megabytes_);
    table_.new_search();
    killers_ = {};
    history_ = {};

    nodes_ = 0;
    quiescence_nodes_ = 0;
    tt_hits_ = 0;
    check_extensions_ = 0;
    quiescence_checks_ = 0;
    selective_depth_ = 0;
    aborted_ = false;
    external_stop_ = nullptr;
    has_deadline_ = limits.move_time.count() > 0 && !limits.infinite;
    if (has_deadline_) deadline_ = started + limits.move_time;

    const int remaining_history =
        chess::PLAY_NB - static_cast<int>(position.play()) - 1;
    const int maximum_depth = std::clamp(limits.depth, 1,
        std::max(1, std::min(64, remaining_history)));

    for (int depth = 1; depth <= maximum_depth; ++depth) {
        chess::Move iteration_best;
        aborted_ = false;
        Score alpha = -SCORE_INFINITE;
        Score beta = SCORE_INFINITE;
        Score window = 50;
        if (depth >= 3 && !is_mate_score(result.score)) {
            alpha = std::max(-SCORE_INFINITE, result.score - window);
            beta = std::min(SCORE_INFINITE, result.score + window);
        }
        Score score = SCORE_DRAW;
        while (true) {
            iteration_best = {};
            score = alpha_beta(current, depth, alpha, beta, 0, 0,
                               &iteration_best);
            if (aborted_ || (score > alpha && score < beta) ||
                (alpha == -SCORE_INFINITE && beta == SCORE_INFINITE)) {
                break;
            }
            window *= 2;
            if (score <= alpha) {
                alpha = std::max(-SCORE_INFINITE, score - window);
            } else {
                beta = std::min(SCORE_INFINITE, score + window);
            }
        }
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

    prepare_worker(limits, started, nullptr);

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

    const int worker_count = std::max(1, std::min(threads_, move_count));
    std::vector<Search*> workers;
    workers.reserve(static_cast<std::size_t>(worker_count));
    const auto worker_hash = std::max<std::size_t>(
        1, hash_megabytes_ / static_cast<std::size_t>(worker_count));
    table_.resize(worker_hash);
    workers.push_back(this);
    const auto auxiliary_count = static_cast<std::size_t>(
        std::max(0, worker_count - 1));
    while (analysis_workers_.size() < auxiliary_count) {
        analysis_workers_.push_back(std::make_unique<Search>(worker_hash));
    }
    if (analysis_workers_.size() > auxiliary_count) {
        analysis_workers_.resize(auxiliary_count);
    }
    for (int index = 1; index < worker_count; ++index) {
        auto& worker = *analysis_workers_[static_cast<std::size_t>(index - 1)];
        worker.resize_hash(worker_hash);
        worker.prepare_worker(limits, started, &stop_requested_);
        workers.push_back(&worker);
    }

    std::vector<std::atomic<std::uint64_t>> worker_nodes(
        static_cast<std::size_t>(worker_count));
    std::vector<std::atomic<std::uint64_t>> worker_qnodes(
        static_cast<std::size_t>(worker_count));
    std::vector<std::atomic<std::uint64_t>> worker_tt_hits(
        static_cast<std::size_t>(worker_count));
    std::vector<std::atomic<int>> worker_seldepth(
        static_cast<std::size_t>(worker_count));

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
        for (int index = 0; index < worker_count; ++index) {
            snapshot.selective_depth = std::max(snapshot.selective_depth,
                worker_seldepth[static_cast<std::size_t>(index)].load(
                    std::memory_order_relaxed));
            snapshot.nodes += worker_nodes[static_cast<std::size_t>(index)].load(
                std::memory_order_relaxed);
            snapshot.quiescence_nodes +=
                worker_qnodes[static_cast<std::size_t>(index)].load(
                    std::memory_order_relaxed);
            snapshot.tt_hits +=
                worker_tt_hits[static_cast<std::size_t>(index)].load(
                    std::memory_order_relaxed);
        }
        snapshot.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return snapshot;
    };

    std::vector<AnalysisLine> iteration_lines;
    result = make_snapshot(iteration_lines, 0, 0, move_count == 0);
    if (callback) callback(result);

    for (int depth = 1; depth <= maximum_depth && move_count > 0; ++depth) {
        std::vector<std::optional<AnalysisLine>> slots(
            static_cast<std::size_t>(move_count));
        std::atomic<int> next_move{0};
        std::atomic<int> completed{0};
        std::mutex publish_mutex;

        auto collect_lines = [&]() {
            std::vector<AnalysisLine> lines;
            lines.reserve(static_cast<std::size_t>(move_count));
            for (const auto& slot : slots) {
                if (slot) lines.push_back(*slot);
            }
            return lines;
        };

        auto publish = [&]() {
            std::lock_guard lock(publish_mutex);
            iteration_lines = collect_lines();
            result = make_snapshot(iteration_lines, depth,
                completed.load(std::memory_order_relaxed),
                completed.load(std::memory_order_relaxed) == move_count);
            if (callback) callback(result);
        };

        auto run_worker = [&](int worker_index) {
            auto& worker = *workers[static_cast<std::size_t>(worker_index)];
            auto publish_worker_stats = [&]() {
                worker_nodes[static_cast<std::size_t>(worker_index)].store(
                    worker.nodes_, std::memory_order_relaxed);
                worker_qnodes[static_cast<std::size_t>(worker_index)].store(
                    worker.quiescence_nodes_, std::memory_order_relaxed);
                worker_tt_hits[static_cast<std::size_t>(worker_index)].store(
                    worker.tt_hits_, std::memory_order_relaxed);
                worker_seldepth[static_cast<std::size_t>(worker_index)].store(
                    worker.selective_depth_, std::memory_order_relaxed);
            };
            worker.heartbeat_ = [&]() {
                publish_worker_stats();
                publish();
            };
            worker.next_heartbeat_ = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(200);
            auto branch = position;
            while (!stop_requested_.load(std::memory_order_relaxed)) {
                const int move_index = next_move.fetch_add(
                    1, std::memory_order_relaxed);
                if (move_index >= move_count) break;

                worker.pv_length_.fill(0);
                worker.aborted_ = false;
                const auto move = moves[move_index];
                branch.make_move(move);
                const Score score = -worker.alpha_beta(branch, depth - 1,
                    -SCORE_INFINITE, SCORE_INFINITE, 1, 0, nullptr);
                const int child_length = worker.pv_length_[1];
                std::vector<chess::Move> cached_variation;
                if (child_length <= 1) {
                    cached_variation =
                        worker.extract_principal_variation(branch, depth - 1);
                }
                branch.undo_move(move);

                publish_worker_stats();
                if (worker.aborted_) break;

                AnalysisLine line;
                line.move = move;
                line.score = score;
                line.depth = depth;
                line.principal_variation.push_back(move);
                for (int index = 1; index < child_length; ++index) {
                    line.principal_variation.push_back(
                        worker.pv_table_[1][static_cast<std::size_t>(index)]);
                }
                if (line.principal_variation.size() == 1U) {
                    line.principal_variation.insert(
                        line.principal_variation.end(),
                        cached_variation.begin(), cached_variation.end());
                }
                {
                    std::lock_guard lock(publish_mutex);
                    slots[static_cast<std::size_t>(move_index)] =
                        std::move(line);
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
                publish();
            }
            worker.heartbeat_ = {};
        };

        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(worker_count));
        for (int index = 0; index < worker_count; ++index) {
            threads.emplace_back(run_worker, index);
        }
        for (auto& thread : threads) thread.join();

        iteration_lines = collect_lines();
        const int completed_count = completed.load(std::memory_order_relaxed);
        if (completed_count == move_count) {
            previous_lines = iteration_lines;
            std::unordered_map<std::uint32_t, Score> root_scores;
            root_scores.reserve(iteration_lines.size());
            for (const auto& line : iteration_lines) {
                root_scores.emplace(line.move.value(), line.score);
            }
            std::stable_sort(moves, moves + move_count,
                [&](chess::Move lhs, chess::Move rhs) {
                    return root_scores.at(lhs.value()) >
                        root_scores.at(rhs.value());
                });
            result = make_snapshot(iteration_lines, depth, completed_count, true);
        } else {
            result = make_snapshot(iteration_lines, depth, completed_count, false);
            if (callback) callback(result);
            break;
        }
        if (should_stop()) break;
    }

    external_stop_ = nullptr;
    return result;
}

Score Search::alpha_beta(chess::Position& position, int depth, Score alpha,
                         Score beta, int ply, int extensions_used,
                         chess::Move* root_best) {
    if (ply >= MAX_PV_PLY - 1) return evaluate(position);
    pv_length_[static_cast<std::size_t>(ply)] = ply;

    if (position.is_repetition()) return SCORE_DRAW;
    const bool in_check = position.in_check();
    const bool fifty_move_draw = position.is_fifty_move_draw();
    if (fifty_move_draw && !in_check) return SCORE_DRAW;

    if (in_check && extensions_used < MAX_CHECK_EXTENSIONS) {
        ++depth;
        ++extensions_used;
        ++check_extensions_;
    }

    if (depth <= 0) {
        return quiescence(position, alpha, beta, ply, 0, extensions_used);
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
        if (!entry->quiescence && entry->depth >= depth &&
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
        const Score terminal = in_check
            ? -SCORE_MATE + ply
            : SCORE_DRAW;
        table_.store(key, depth, extensions_used, score_to_table(terminal, ply),
                     TranspositionTable::Bound::Exact, chess::Move{});
        return terminal;
    }
    if (fifty_move_draw) return SCORE_DRAW;

    const auto color_index = static_cast<std::size_t>(position.turn().id());
    const auto* history = history_[color_index].data();
    const auto* killers = ply < MAX_PV_PLY
        ? &killers_[static_cast<std::size_t>(ply)]
        : nullptr;
    order_moves(position, moves, move_count, table_move, true, killers,
                history);

    Score best = -SCORE_INFINITE;
    chess::Move best_move;
    for (int i = 0; i < move_count; ++i) {
        const auto move = moves[i];
        position.make_move(move);
        Score score;
        if (i == 0) {
            score = -alpha_beta(position, depth - 1, -beta, -alpha,
                                ply + 1, extensions_used, nullptr);
        } else {
            score = -alpha_beta(position, depth - 1, -alpha - 1, -alpha,
                                ply + 1, extensions_used, nullptr);
            if (!aborted_ && score > alpha && score < beta) {
                score = -alpha_beta(position, depth - 1, -beta, -alpha,
                                    ply + 1, extensions_used, nullptr);
            }
        }
        position.undo_move(move);

        if (aborted_) return SCORE_DRAW;

        if (score > best) {
            best = score;
            best_move = move;
            update_principal_variation(ply, move);
            if (root_best != nullptr) *root_best = move;
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            if (!is_noisy(position, move)) {
                auto& ply_killers = killers_[static_cast<std::size_t>(ply)];
                if (move != ply_killers[0]) {
                    ply_killers[1] = ply_killers[0];
                    ply_killers[0] = move;
                }
                const auto history_index =
                    static_cast<std::size_t>(move.source()) * chess::SQUARE_NB +
                    static_cast<std::size_t>(move.target());
                auto& history_entry = history_[color_index][history_index];
                history_entry = std::min<std::int32_t>(1'000'000,
                    history_entry + depth * depth);
            }
            break;
        }
    }

    auto bound = TranspositionTable::Bound::Exact;
    if (best <= original_alpha) bound = TranspositionTable::Bound::Upper;
    if (best >= beta) bound = TranspositionTable::Bound::Lower;
    table_.store(key, depth, extensions_used, score_to_table(best, ply), bound,
                 best_move);

    return best;
}

Score Search::quiescence(chess::Position& position, Score alpha, Score beta,
                         int ply, int quiescence_ply,
                         int extensions_used) {
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

    if (position.is_repetition()) return SCORE_DRAW;
    const bool in_check = position.in_check();
    const bool fifty_move_draw = position.is_fifty_move_draw();
    if (fifty_move_draw && !in_check) return SCORE_DRAW;

    const auto key = position.key();
    const Score original_alpha = alpha;
    chess::Move table_move;
    if (const auto* entry = table_.probe(key)) {
        ++tt_hits_;
        table_move = entry->best_move;
        const Score table_score = score_from_table(entry->score, ply);
        const int remaining_quiescence =
            MAX_QUIESCENCE_PLY - quiescence_ply;
        const bool deep_enough = entry->covers_quiescence(
            remaining_quiescence, extensions_used);
        const bool cutoff = deep_enough && (
            entry->bound == TranspositionTable::Bound::Exact ||
            (entry->bound == TranspositionTable::Bound::Lower &&
             table_score >= beta) ||
            (entry->bound == TranspositionTable::Bound::Upper &&
             table_score <= alpha));
        if (cutoff) return table_score;
    }
    if (quiescence_ply >= MAX_QUIESCENCE_PLY) {
        return evaluate(position) - (in_check ? 50 : 0);
    }
    chess::Move moves[chess::MOVE_NB];
    int move_count = 0;
    Score stand_pat = -SCORE_INFINITE;

    if (in_check) {
        move_count = chess::generate_legal_moves(position, moves);
        if (move_count == 0) {
            const Score mate = -SCORE_MATE + ply;
            table_.store(key, 0, 0,
                score_to_table(mate, ply), TranspositionTable::Bound::Exact,
                chess::Move{}, true, MAX_QUIESCENCE_PLY - quiescence_ply);
            return mate;
        }
        if (fifty_move_draw) return SCORE_DRAW;
    } else {
        stand_pat = evaluate(position);
        if (stand_pat >= beta) {
            table_.store(key, 0, 0,
                score_to_table(stand_pat, ply),
                TranspositionTable::Bound::Lower, chess::Move{}, true,
                MAX_QUIESCENCE_PLY - quiescence_ply);
            return stand_pat;
        }
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

    order_moves(position, moves, move_count, table_move, false);

    chess::Move best_move;
    for (int i = 0; i < move_count; ++i) {
        const auto move = moves[i];
        const bool delta_candidate = !in_check &&
            move.policy() != chess::Move::Policy::Evolve &&
            stand_pat + tactical_gain(position, move) + DELTA_MARGIN < alpha;
        const auto mover = position.turn();
        position.make_move(move);
        if (delta_candidate && !position.in_check() &&
            !position.in_check(mover.prev().id())) {
            position.undo_move(move);
            continue;
        }
        const Score score = -quiescence(position, -beta, -alpha, ply + 1,
                                        quiescence_ply + 1, extensions_used);
        position.undo_move(move);

        if (aborted_) return SCORE_DRAW;
        if (score >= beta) {
            update_principal_variation(ply, move);
            table_.store(key, 0, 0,
                score_to_table(score, ply), TranspositionTable::Bound::Lower,
                move, true, MAX_QUIESCENCE_PLY - quiescence_ply);
            return score;
        }
        if (score > alpha) {
            alpha = score;
            best_move = move;
            update_principal_variation(ply, move);
        }
    }

    const auto bound = alpha > original_alpha
        ? TranspositionTable::Bound::Exact
        : TranspositionTable::Bound::Upper;
    table_.store(key, 0, 0, score_to_table(alpha, ply), bound, best_move,
                 true, MAX_QUIESCENCE_PLY - quiescence_ply);
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
