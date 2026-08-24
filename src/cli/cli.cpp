#include <chrono>
#include <algorithm>
#include <sstream>
#include "cli/cli.h"
#include "misc.h"

namespace athena::cli {

CLI::CLI() {
    registerCommands();
}

void CLI::registerCommands() {
    commands_["uci"]        = [this](std::istream& is) { uci(is); };
    commands_["isready"]    = [this](std::istream& is) { isready(is); };
    commands_["setoption"]  = [this](std::istream& is) { setoption(is); };
    commands_["ucinewgame"] = [this](std::istream& is) { ucinewgame(is); };
    commands_["position"]   = [this](std::istream& is) { pos(is); };
    commands_["pos"]        = [this](std::istream& is) { pos(is); };
    commands_["go"]         = [this](std::istream& is) { go(is); };
    commands_["stop"]       = [this](std::istream& is) { stop(is); };
    commands_["analyze"]    = [this](std::istream& is) { analyze(is); };
    commands_["state"]      = [this](std::istream& is) { state(is); };
    commands_["play"]       = [this](std::istream& is) { play(is); };
    commands_["undo"]       = [this](std::istream& is) { undo(is); };
    commands_["quit"]       = [this](std::istream& is) { quit(is); };
    commands_["perft"]      = [this](std::istream& is) { perft(is); };
    commands_["print"]      = [this](std::istream& is) { print(is); };
}

int CLI::run(int argc, char** argv) {

    // Handle command-line arguments
    if (argc > 1)
    {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Athena " << misc::version() << "\n"
                << "A free uci-compatible 4-player chess engine.\n"
                << "For full documentation, read the README.md file or visit the GitHub page: "
                << misc::github_url() 
                << std::endl;
            return EXIT_SUCCESS;
        }
        else if (arg == "--version" || arg == "-v") {
            std::cout
                << "Athena " << misc::version()
                << std::endl;
            return EXIT_SUCCESS;
        }
        else if (arg == "--license") {
            std::cout 
                << "Athena is free and open source software licensed under the MIT License.\n"
                << "See the LICENSE file for details."
                << std::endl;
            return EXIT_SUCCESS;
        }
        else {
            std::cerr << "info string unknown command line argument '" << arg << "'" << std::endl;
            return EXIT_FAILURE;
        }
    }

    // UCI interactive loop
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::istringstream is(line);
        std::string cmd;
        is >> cmd;

        auto it = commands_.find(cmd);
        if (it != commands_.end()) {
            it->second(is);
        } else {
            std::cout << "info string unknown command '" << line << "'\n";
        }
    }

    return EXIT_SUCCESS;
}

void CLI::uci(std::istream&) {
    std::cout << "id name Athena " << misc::version() << "\n";
    std::cout << "id author " << misc::author() << "\n";
    std::cout << "option name Setup type combo default modern var modern var classic\n";
    std::cout << "option name Hash type spin default 16 min 1 max 1024\n";
    std::cout << "option name AnalysisThreads type spin default 1 min 1 max 4\n";
    std::cout << "uciok" << std::endl;
}

void CLI::isready(std::istream&) {
    std::cout << "readyok" << std::endl;
}

void CLI::setoption(std::istream& args) {

    std::string token, name, value;

    for (int i=0; i<4; i++) {
        if (!(args >> token)) {
            std::cout << "info string expected format: setoption name <name> value <value>\n";
            return;
        }
        if (i == 1) name  = token;
        if (i == 3) value = token;
    }

    engine_.setOption(name, value);
}

void CLI::ucinewgame(std::istream&) {
    engine_.newGame();
}

void CLI::pos(std::istream& args) {

    std::string token;
    if (!(args >> token)) {
        std::cout << "info string position expected startpos or fen\n";
        return;
    }

    if (token == "startpos") {
        if (args >> token && (token == "modern" || token == "classic")) {
            engine_.setSetup(token);
            args >> token;
        }
        engine_.newGame();
    } else if (token == "fen") {
        std::string fen;
        if (!(args >> fen)) {
            std::cout << "info string expected FEN string after 'fen' command\n";
            return;
        }
        engine_.setPosition(fen);
        args >> token;
    } else {
        std::cout << "info string position expected startpos or fen\n";
        return;
    }

    if (token == "moves") {
        while (args >> token) {
            if (!engine_.applyMove(token)) {
                std::cout << "info string illegal move: " << token << '\n';
                return;
            }
        }
    }
}

void CLI::go(std::istream& args) {
    core::Search::Limits limits;
    bool has_explicit_depth = false;
    std::string token;
    while (args >> token) {
        if (token == "depth") {
            args >> limits.depth;
            has_explicit_depth = true;
        } else if (token == "movetime") {
            std::int64_t milliseconds = 0;
            args >> milliseconds;
            limits.move_time = std::chrono::milliseconds(std::max<std::int64_t>(1, milliseconds));
        } else if (token == "infinite") {
            limits.infinite = true;
            limits.depth = 64;
        }
    }
    if (limits.move_time.count() > 0 && !has_explicit_depth) {
        limits.depth = 64;
    }
    engine_.go(limits);
}

void CLI::stop(std::istream&) {
    engine_.stop();
}

void CLI::analyze(std::istream& args) {
    core::Search::Limits limits;
    std::size_t max_lines = 8;
    std::uint64_t analysis_id = 0;
    std::string token;
    while (args >> token) {
        if (token == "depth") {
            args >> limits.depth;
        } else if (token == "movetime") {
            std::int64_t milliseconds = 0;
            args >> milliseconds;
            limits.move_time = std::chrono::milliseconds(
                std::max<std::int64_t>(0, milliseconds));
        } else if (token == "multipv") {
            args >> max_lines;
        } else if (token == "id") {
            args >> analysis_id;
        } else if (token == "infinite") {
            limits.infinite = true;
            limits.depth = 64;
        }
    }
    max_lines = std::clamp<std::size_t>(max_lines, 1, 64);
    engine_.analyze(limits, max_lines, analysis_id);
}

void CLI::state(std::istream&) {
    engine_.state();
}

void CLI::play(std::istream& args) {
    std::string move;
    if (!(args >> move)) {
        std::cout << "play error missing-move\n";
    } else if (engine_.applyMove(move)) {
        std::cout << "play ok " << move << '\n';
    } else {
        std::cout << "play illegal " << move << '\n';
    }
    engine_.state();
}

void CLI::undo(std::istream&) {
    std::cout << (engine_.undoMove() ? "undo ok\n" : "undo empty\n");
    engine_.state();
}

void CLI::quit(std::istream&) {
    engine_.stop();
    std::exit(EXIT_SUCCESS);
}

void CLI::perft(std::istream& args) {
    int depth;
    if (!(args >> depth) || depth < 0) {
        std::cout << "Usage: perft <depth> [--split]\n";
        return;
    }

    bool split = false;
    std::string arg;

    if (args >> arg) {
        if (arg == "--split") {
            split = true;

            // No more arguments allowed.
            if (args >> arg) {
                std::cout << "Usage: perft <depth> [--split]\n";
                return;
            }
        } else {
            std::cout << "Usage: perft <depth> [--split]\n";
            return;
        }
    }

    if (split && depth == 0) {
        std::cout << "Usage: perft <positive-depth> --split\n";
        return;
    }
    engine_.perft(depth, split);
}

void CLI::print(std::istream& args) {
    std::string arg;
    if (!(args >> arg)) {
        engine_.print(false);
    } else if (arg == "--board16x16") {
        engine_.print(true);
    } else {
        std::cout << " Usage: print [--board16x16]\n\n";
        return;
    }
}

} // namespace athena
