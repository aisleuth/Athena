# Athena Project Handoff

Updated: 2026-09-04

## Project identity

- Repository: `https://github.com/aisleuth/Athena`
- Upstream: `https://github.com/arianahejazyan/Athena`
- Local path: `/Users/johnphilippides/Documents/4p Chess/Athena`
- Working branch at handoff: `codex/complete-engine`
- Target branch on GitHub: `main`
- Game: team four-player chess on a 14x14 cross board
- Teams: Red + Yellow versus Blue + Green

Athena began as Ariana Hejazyan's four-player chess move generator and UCI-style
engine. This fork adds a complete hand-written analysis engine and a local web
GUI. It is intended for local, manual analysis and practice; it does not connect
to or automate play on Chess.com.

## Current capabilities

### Search

- Team-aware negamax with alpha-beta pruning
- Iterative deepening, principal-variation search, and aspiration windows
- Root MultiPV analysis with continuously updated candidate rankings
- One to four root-analysis worker threads
- Transposition move, capture, promotion, check, killer, and history ordering
- Quiescence search with bounded quiet checks, check evasions, delta pruning,
  and capture filtering
- Bounded check extensions for coordinated mating combinations
- Full principal-variation reporting
- Incremental Zobrist hashing
- Clustered, generation- and depth-aware transposition table, including
  quiescence entries
- Threefold-repetition and four-player fifty-move draw detection
- Fast direct/discovered-check classification without make/undo in the hot
  move-ordering loop
- Fixed-depth, fixed-time, unlimited, and interruptible analysis

### Evaluation

- Team-aware material
- Color-relative piece-square and pawn-advancement terms
- Central activity
- Pawn shelter around both allied kings
- Pressure against either opposing king
- Incrementally maintained material and piece-square totals
- Hand-tuned evaluation only; there is no trained neural network

### GUI

- Full interactive 14x14 board
- Simple click-to-select, click-to-move input with legal targets
- Best-move arrow, ranked alternatives, scores, and principal variations
- Live depth, time, node count, and current root-pass progress
- Depth, time, MultiPV, hash, setup, and one-to-four-thread controls
- Unlimited analysis when search time is zero
- Checks and captures prioritized at the root
- Undo, reset, play-best, automatic refresh, and optional engine reply
- Ninety-degree board rotation so any color can be placed at the bottom
- Refresh recovery: requesting board state interrupts a stale unlimited search,
  preventing a blank board after the page is reloaded

### Practice mode

The user chooses Red + Yellow or Blue + Green. On a selected-team turn, Athena
analyzes privately and hides the arrow, candidates, and evaluations. Once the
user commits a move, the GUI reveals:

- Athena's preferred move
- The evaluation after the user's move
- The score lost relative to the preferred move
- The normal best-move arrow and candidate list

The `Opponent analysis ->` button then advances to visible analysis for the
opposing player. The server and CLI allow up to the complete legal move count
for MultiPV so a played practice move can be scored even when it ranks below
the former 64-line limit.

## Code map

- `src/chess/`: board representation, attacks, move generation, position
  state, make/undo, piece-square tables, and Zobrist hashing
- `src/core/search/`: search, evaluation, move ordering, time management,
  worker threads, and transposition table
- `src/core/engine/`: engine facade used by the CLI
- `src/cli/`: UCI-style protocol plus GUI-specific state/analysis commands
- `gui/server.mjs`: dependency-free local HTTP server and persistent native
  engine process
- `gui/public/`: browser UI
- `tests/`: perft, position, hashing, evaluation, transposition-table, and
  search regression tests
- `tools/bench.sh`: fixed-position strength/behavior gate
- `tools/bench_expected.txt`: expected best moves and scores
- `run-gui.sh`: build-if-needed GUI launcher

## Build, run, and validate

Requirements: CMake 3.25+, a C++20 compiler, and Node.js 18+ for the GUI.

```bash
cd "/Users/johnphilippides/Documents/4p Chess/Athena"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./run-gui.sh
```

Open `http://127.0.0.1:8787` if the launcher does not open it automatically.
The native CLI is `./build/src/athena`.

Validation commands:

```bash
ctest --test-dir build --output-on-failure
tools/bench.sh --check
node --check gui/server.mjs
node --check gui/public/app.js
```

Important benchmarking note: incremental builds have previously lost LTO and
produced misleading timings. Before trusting a performance comparison, perform
a clean Release configuration and build. Never accept a search optimization
from speed alone: run the tests and `tools/bench.sh --check`, and explicitly
review any intended best-move or score changes.

## Current verified state

At this handoff:

- All 44 CTest tests pass.
- `tools/bench.sh --check` matches the versioned reference exactly.
- Both GUI JavaScript entry points pass `node --check`.
- The latest work adds practice mode, removes the 64-line analysis ceiling,
  and fixes blank-board reloads after unlimited analysis.

## Engineering decisions and cautions

- Null-move pruning is deliberately avoided. In four-player chess, passing the
  turn hands a real move to an opponent, so the usual two-player assumption is
  unsafe.
- Quiet-check quiescence is deliberately bounded because checks are much denser
  with four kings and previously consumed nearly the entire search tree.
- `Position::gives_check(move, info)` is the preferred per-move fast path.
  `would_check()` remains the correctness fallback for positions already in
  check and for exceptional moves such as castling, en passant, and promotion.
- `AnalysisThreads` parallelizes ranked root analysis only. The ordinary UCI
  `go` path remains single-threaded.
- Search-behavior changes should be introduced one at a time and measured.

## Suggested next work

1. Add conservative late-move reductions (LMR), initially only for sufficiently
   late, quiet, non-checking moves at adequate depth. Re-search any reduced move
   that raises alpha. Gate the change with tests, benchmark behavior, and
   measured time-to-depth rather than expecting identical node counts.
2. Investigate lazy evaluation using the incremental piece-square/material
   subtotal, with generous margins so tactical or king-pressure terms are not
   skipped near alpha/beta boundaries.
3. Improve history-table locality, potentially moving from a large
   source-target table to a piece-target representation after confirming search
   quality is preserved.
4. Build a repeatable strength harness: engine-versus-engine matches across
   fixed openings, colors, time controls, and alternating teams. Report Elo with
   uncertainty rather than trying to infer a Chess.com rating from depth or
   nodes alone.
5. Treat neural-network/self-play work as a separate long-term track. The
   existing legal move generator, position representation, search, GUI, and
   match harness would remain useful, but data generation and training
   infrastructure would be substantial new work.

## Prompt for the next Codex task

Read this file and `README.md`, inspect the current Git status and latest commit,
then continue Athena development from the verified state above. Preserve the
four-player team semantics, avoid null-move pruning, keep unrelated user changes,
and run both CTest and `tools/bench.sh --check` after search changes. Begin by
confirming the repository is clean and that the requested next feature is still
the user's priority.
