<div align="center">

  <img src="./assets/logo.png" alt="Athena Chess Engine Logo" style="width: 250px; height:250px; border-radius: 10px;"/>

  <h3>Athena</h3>

  A UCI-compatible four-player chess engine

</div>
<div align="center">
<img src="./assets/wallpaper.png" alt="Four Player Chess Board" style="width: 100%; height:auto; border-radius: 10px;"/>
</div>

## Table of Contents
- [Introduction](#introduction)
- [Getting Started](#getting-started)
- [Commands](#commands)
- [Features](#features)
- [4PC Engines](#4pc-engines)
- [Acknowledgements](#acknowledgements)
## Introduction
Athena is a local engine for team four-player chess. Red and Yellow form one
team; Blue and Green form the other. It provides legal move generation for the
14x14 cross board and a team-aware alpha-beta search that can be driven
through a UCI-style command-line protocol.

This fork builds on [Ariana Hejazyan's original Athena engine](https://github.com/arianahejazyan/Athena)
and adds a complete search stack and local analysis GUI: positional evaluation,
quiescence search, a transposition table, coordinated-mate extensions, live
iterative MultiPV analysis, an interactive board, and continuously updating
rankings, progress, and best-move arrows.
## Getting Started
To build Athena from source, run the following commands:
```bash
git clone https://github.com/aisleuth/Athena.git
cd Athena
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
Launch the interactive CLI with:
```
./build/src/athena
```

### Local graphical interface

Athena includes a dependency-free local browser interface for analysis. After
cloning the repository, the launcher will build the native engine when needed
and start the GUI:

```bash
./run-gui.sh
```

Alternatively, build the engine yourself and start the Node.js 18+ server:

```bash
node gui/server.mjs
```

Then open [http://127.0.0.1:8787](http://127.0.0.1:8787). The GUI provides the
full 14x14 cross board, two-click movement with legal-move highlighting,
a best-move arrow, ranked candidate evaluations and principal variations,
undo/reset controls, and adjustable depth, search time, MultiPV, hash size,
analysis threads (one to four), and starting setup. Analysis automatically
refreshes after a move by default; an optional setting lets Athena play one
best response as well.

Ranked analysis uses iterative deepening and updates the move list, ratings,
principal variations, and best-move arrow while the search is running. A
search time of zero means unlimited analysis: Athena keeps advancing to deeper
passes until `Stop` is pressed. The progress meter shows exact root-move
completion for the current depth pass and live node/time totals; it resets as
the engine enters each deeper pass. Checks and captures are searched before
quiet root moves.

The `Threads` control parallelizes candidate root moves during ranked GUI
analysis. One thread is the default for lower heat and power use; two to four
threads can finish a depth pass faster on multi-core computers. The selected
hash budget is divided across the analysis workers.

Use `ATHENA_GUI_PORT` to select a different port or `ATHENA_ENGINE` to point at
a non-default Athena executable.

Run the test suite with:
```bash
ctest --test-dir build --output-on-failure
```

Athena's move generator is highly optimized, achieving approximately 120 Mnps (million nodes per second) in benchmarks. You can verify this yourself:
```bash
./build/tests/perft_bench --benchmark_counters_tabular=true
```

## Commands

Athena supports the core UCI workflow plus four-player position and setup
extensions. It is not yet a drop-in replacement for every two-player UCI
option.

### UCI Commands

| Command       | Description                                                                 |
|---------------|-----------------------------------------------------------------------------|
| `uci`         | Identifies the engine and returns its name, version, and supported options. |
| `isready`     | Checks if the engine is ready; responds with `readyok` when synchronized.   |
| `setoption`   | Sets a configuration option such as `Setup`, `Hash`, or `Threads`.          |
| `ucinewgame`  | Notifies the engine that a new game is about to begin.                      |
| `position`    | Sets `startpos` or a four-player FEN, optionally followed by legal moves.   |
| `go`          | Starts an asynchronous search (`depth`, `movetime`, or `infinite`).          |
| `stop`        | Stops the current search as soon as possible.                               |
| `quit`        | Shuts down the engine.                                                      |

Example session:
```text
uci
isready
position startpos modern moves e1d3 a5c4
go depth 5
```

### UCI Options

| Name      | Default  | Description                                                        |
|-----------|----------|--------------------------------------------------------------------|
| `Setup`   | `modern` | Board setup variant to use (`modern` or `classic`).                |
| `Hash`    | `16`     | Transposition-table size in MiB (`1` to `1024`).                    |
| `Threads` | `1`      | Root-analysis worker threads (`1` to `4`; ranked analysis only).    |

### Debug Commands

Athena also provides additional commands for testing and debugging:

| Command | Description                                                                      |
|---------|----------------------------------------------------------------------------------|
| `perft` | Runs a perft (performance test) to count legal moves at a given depth.           |
| `print` | Displays the current board position in the console.

## Features
### Chess
- UCI protocol
- 256-bit bitboard board representation
- Optimized legal move generator (~120 Mnps)
### Search
- Negamax
- Alpha-Beta pruning
- Principal-variation search with aspiration windows and iterative deepening
- Move ordering using transposition moves, captures, promotions, checks, killer moves, and history scores
- Quiescence search through captures, promotions, forcing checks, and check evasions, with delta pruning and a bounded quiet-check horizon
- Bounded check extensions for tactical and coordinated mating lines
- Complete principal-variation output instead of only the root move
- Incrementally maintained Zobrist keys instead of recomputing the position hash at every node
- Generation- and depth-aware transposition table with exact, lower, and upper bounds, including quiescence entries
- Threefold-repetition and 50-complete-move draw detection for four-player turn order
- Fixed-depth, fixed-movetime, infinite, and interruptible searches
- Parallel ranked root analysis with one to four worker threads
### Evaluate
- Team-aware material counting
- Central piece activity
- Color-relative pawn advancement
- Pawn shelter around both allied kings
- Check pressure against either opposing king, including the opponent who moves later

### Current limitations
- The standard `go` command is single-threaded; `Threads` applies to ranked analysis
- Hand-tuned evaluation rather than a trained network
- No built-in Chess.com connection (the local graphical interface is manual)
- Team mode only; free-for-all scoring and elimination are not implemented
## 4PC Engines
This is a list of active four-player chess (4PC) engines. Feel free to add your own engine here or ask me to include it. You can also find a list of four-player chess tools in the [Colosseum](https://github.com/arianahejazyan/Colosseum).
 
| Engine        | GitHub                                      | Chess.com                          |
|---------------|---------------------------------------------|------------------------------------|
| Athena        | [arianahejazyan/Athena](https://github.com/arianahejazyan/Athena) | [TeamAthena](https://www.chess.com/member/teamathena1) |
| Samaritan     | [Moxile/Samaritan](https://github.com/Moxile/Samaritan) | —                                  |
| Enigma        | [anurag-baundwal/4pchess](https://github.com/anurag-baundwal/4pchess) | [TeamEnigma](https://www.chess.com/member/teamenigma1) |
| Nexus         | (private)                                   | [TeamNexus](https://www.chess.com/member/TeamNexus1) |
| BlackKnight   | (private)                                   | [TeamBlackKnight](https://www.chess.com/member/TeamBlackKnight1) |
| Titan         | [obryanlouis/4pchess](https://github.com/obryanlouis/4pchess) | [TeamTitan](https://www.chess.com/member/teamtitan1) |
| Terminator    | (private)                                   | [TeamTerminator](https://www.chess.com/member/teamterminator1) |

## Acknowledgements
- **Special thanks** to Moxile for always supporting me and for the many hours of programming together.
- **Special thanks** to tsoj, Lotfy and the Discord chess engine community for their help with coding and discussions.
- **Special thanks** to qilp and Chess.com team for providing API access and support.
- **Special thanks** to all contributors for their support and contributions.
