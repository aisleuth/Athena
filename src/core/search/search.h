#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>
#include "chess/move.h"
#include "chess/position.h"
#include "evaluate.h"
#include "tt.h"

namespace athena::core {

class Search {
public:
    explicit Search(std::size_t hash_megabytes = 16);

    struct Limits {
        int depth = 4;
        std::chrono::milliseconds move_time{0};
        bool infinite = false;
    };

    struct Result {
        chess::Move best_move{};
        Score score = SCORE_DRAW;
        int depth = 0;
        int selective_depth = 0;
        std::uint64_t nodes = 0;
        std::uint64_t quiescence_nodes = 0;
        std::uint64_t tt_hits = 0;
        std::uint64_t check_extensions = 0;
        std::uint64_t quiescence_checks = 0;
        std::vector<chess::Move> principal_variation;
        std::chrono::milliseconds elapsed{0};
    };

    struct AnalysisLine {
        chess::Move move{};
        Score score = SCORE_DRAW;
        int depth = 0;
        std::vector<chess::Move> principal_variation;
    };

    struct AnalysisResult {
        std::vector<AnalysisLine> lines;
        int depth = 0;
        int root_moves_completed = 0;
        int root_move_count = 0;
        bool iteration_complete = false;
        int selective_depth = 0;
        std::uint64_t nodes = 0;
        std::uint64_t quiescence_nodes = 0;
        std::uint64_t tt_hits = 0;
        std::chrono::milliseconds elapsed{0};
    };

    using AnalysisCallback = std::function<void(const AnalysisResult&)>;

    void reset() noexcept;
    void stop() noexcept;
    void clear_hash() noexcept { table_.clear(); }
    void resize_hash(std::size_t megabytes) {
        hash_megabytes_ = megabytes == 0 ? 1 : megabytes;
        table_.resize(hash_megabytes_);
    }
    void set_threads(int threads) noexcept;
    int threads() const noexcept { return threads_; }
    Result think(const chess::Position& position, const Limits& limits);
    AnalysisResult analyze(const chess::Position& position,
                           const Limits& limits, std::size_t max_lines,
                           const AnalysisCallback& callback = {});

private:
    Score alpha_beta(chess::Position& position, int depth, Score alpha,
                     Score beta, int ply, int extensions_used,
                     chess::Move* root_best);
    Score quiescence(chess::Position& position, Score alpha, Score beta,
                     int ply, int quiescence_ply);
    std::vector<chess::Move> extract_principal_variation(
        const chess::Position& position, int max_plies) const;
    void update_principal_variation(int ply, chess::Move move) noexcept;
    bool should_stop() noexcept;
    void prepare_worker(const Limits& limits,
                        std::chrono::steady_clock::time_point started,
                        const std::atomic_bool* external_stop) noexcept;

    std::atomic_bool stop_requested_{false};
    const std::atomic_bool* external_stop_{nullptr};
    bool aborted_{false};
    bool has_deadline_{false};
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point next_heartbeat_{};
    std::function<void()> heartbeat_{};
    std::uint64_t nodes_{0};
    std::uint64_t quiescence_nodes_{0};
    std::uint64_t tt_hits_{0};
    std::uint64_t check_extensions_{0};
    std::uint64_t quiescence_checks_{0};
    int selective_depth_{0};
    static constexpr int MAX_PV_PLY = 96;
    std::array<std::array<chess::Move, MAX_PV_PLY>, MAX_PV_PLY> pv_table_{};
    std::array<int, MAX_PV_PLY> pv_length_{};
    std::array<std::array<chess::Move, 2>, MAX_PV_PLY> killers_{};
    std::array<std::array<std::int32_t,
        chess::SQUARE_NB * chess::SQUARE_NB>, chess::COLOR_NB> history_{};
    TranspositionTable table_;
    std::size_t hash_megabytes_{16};
    int threads_{1};
};

} // namespace athena
