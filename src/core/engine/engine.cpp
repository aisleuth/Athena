#include <iostream>
#include <memory>
#include <algorithm>
#include <cctype>
#include "engine.h"
#include "chess/position.h"
#include "chess/castle.h"
#include "chess/constants.h"
#include "chess/movegen.h"
#include "chess/perft.h"
#include "option/option_combo.h"
#include "option/option_spin.h"

namespace athena::core {

Engine::Engine() {
    setSetup("modern");
    setPosition(chess::Position::startpos(pos_.setup()));
    options_["setup"] = std::make_unique<ComboOption>("setup", "modern",
        std::vector<std::string>{"modern", "classic"},
        [this](const Option& o) {
            setSetup(static_cast<const ComboOption&>(o).getValue());
        });
    options_["hash"] = std::make_unique<SpinOption>("hash", 16, 1, 1024,
        [this](const Option& o) {
            stop();
            search_.resize_hash(static_cast<std::size_t>(
                static_cast<const SpinOption&>(o).getValue()));
        });
}

Engine::~Engine() {
    stop();
}

void Engine::setOption(const std::string& name, const std::string& value) {
    std::string normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = options_.find(normalized);
    if (it != options_.end())
        it->second->setValue(value);
    else
        std::cout << "info string unknown option: " << name << "\n";
}

void Engine::setSetup(const std::string& setup) {
    stop();
    setup_ = setup == "classic" ? "classic" : "modern";
    pos_.set_setup(setup_ == "modern" ?
        chess::Castle::Setup::Modern : 
        chess::Castle::Setup::Classic);
}

void Engine::setPosition(const std::string& fen) {
    stop();
    pos_.init(fen);
    move_history_.clear();
}

bool Engine::applyMove(const std::string& move) {
    stop();
    chess::Move legal_moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(pos_, legal_moves);
    for (int i = 0; i < count; ++i) {
        if (legal_moves[i].uci() == move) {
            pos_.make_move(legal_moves[i]);
            move_history_.push_back(legal_moves[i]);
            return true;
        }
    }
    return false;
}

bool Engine::undoMove() {
    stop();
    if (move_history_.empty()) return false;
    const auto move = move_history_.back();
    move_history_.pop_back();
    pos_.undo_move(move);
    return true;
}

void Engine::newGame() {
    stop();
    search_.clear_hash();
    pos_.set_setup(setup_ == "modern"
        ? chess::Castle::Setup::Modern
        : chess::Castle::Setup::Classic);
    pos_.init(chess::Position::startpos(pos_.setup()));
    move_history_.clear();
}

void Engine::analyze(core::Search::Limits limits, std::size_t max_lines,
                     std::uint64_t analysis_id) {
    stop();
    search_.reset();
    const auto position = pos_;
    search_thread_ = std::thread(
        [this, position, limits, max_lines, analysis_id]() {
        std::cout << "analysis " << analysis_id << " begin" << std::endl;
        auto publish = [analysis_id](const core::Search::AnalysisResult& result) {
            std::cout << "analysis " << analysis_id
                      << " update depth " << result.depth
                      << " completed " << result.root_moves_completed
                      << " total " << result.root_move_count
                      << " complete " << (result.iteration_complete ? 1 : 0)
                      << '\n';
        std::size_t rank = 1;
        for (const auto& line : result.lines) {
            std::cout << "analysis " << analysis_id << " line " << rank++
                      << " move " << line.move.uci()
                      << " score ";
            if (is_mate_score(line.score)) {
                std::cout << "mate " << mate_in(line.score);
            } else {
                std::cout << "cp " << line.score;
            }
            std::cout << " depth " << line.depth << " pv";
            for (const auto move : line.principal_variation) {
                std::cout << ' ' << move.uci();
            }
            std::cout << '\n';
        }
        std::cout << "analysis " << analysis_id
                  << " stats nodes " << result.nodes
                  << " qnodes " << result.quiescence_nodes
                  << " tthits " << result.tt_hits
                  << " seldepth " << result.selective_depth
                  << " time " << result.elapsed.count() << '\n'
                  << "analysis " << analysis_id << " update end" << std::endl;
        };
        search_.analyze(position, limits, max_lines, publish);
        std::cout << "analysis " << analysis_id << " end" << std::endl;
    });
}

void Engine::go(core::Search::Limits limits) {
    stop();
    search_.reset();
    const auto position = pos_;
    search_thread_ = std::thread([this, position, limits]() {
        search_.think(position, limits);
    });
}

void Engine::stop() {
    search_.stop();
    if (search_thread_.joinable()) search_thread_.join();
}

void Engine::perft(int depth, bool split) {
    stop();
    auto position = pos_;
    if (!split) {
        std::cout << "nodes " << chess::perft(position, depth) << std::endl;
        return;
    }

    std::uint64_t total = 0;
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(position, moves);
    for (int i = 0; i < count; ++i) {
        position.make_move(moves[i]);
        const auto nodes = chess::perft(position, depth - 1);
        position.undo_move(moves[i]);
        total += nodes;
        std::cout << moves[i].uci() << ": " << nodes << '\n';
    }
    std::cout << "nodes " << total << std::endl;
}

void Engine::state() {
    stop();
    chess::Move moves[chess::MOVE_NB];
    const int count = chess::generate_legal_moves(pos_, moves);
    std::cout << "state begin\n"
              << "state fen " << pos_.fen() << '\n'
              << "state turn " << pos_.turn().uci() << '\n'
              << "state setup " << setup_ << '\n'
              << "state legal";
    for (int index = 0; index < count; ++index) {
        std::cout << ' ' << moves[index].uci();
    }
    std::cout << "\nstate end" << std::endl;
}

} // namespace athena
