#pragma once

#include <memory>
#include <string>
#include <map>
#include <thread>
#include <vector>
#include "option/option.h"
#include "chess/position.h"
#include "core/search/search.h"

namespace athena::core {

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void setOption(const std::string& name, const std::string& value);
    void setPosition(const std::string& fen);
    void setSetup(const std::string& setup);
    bool applyMove(const std::string& move);
    bool undoMove();
    void newGame();

    void go(core::Search::Limits limits);
    void analyze(core::Search::Limits limits, std::size_t max_lines,
                 std::uint64_t analysis_id = 0);
    void stop();
    void perft(int depth, bool split = false);
    void state();

    void print(bool board16x16 = false) { pos_.print(board16x16); }

private:
    chess::Position pos_;
    std::string setup_{"modern"};
    std::map<std::string, std::unique_ptr<Option>> options_;
    core::Search search_;
    std::thread search_thread_;
    std::vector<chess::Move> move_history_;
};

} // namespace athena
