const $ = (selector) => document.querySelector(selector);
const boardElement = $("#board");
const arrow = $("#bestMoveArrow");
const analysisList = $("#analysisList");
const historyElement = $("#moveHistory");

const colors = { r: "Red", b: "Blue", y: "Yellow", g: "Green" };
const teams = { r: "Red team", y: "Red team", b: "Blue team", g: "Blue team" };
const practiceTeams = { ry: ["r", "y"], bg: ["b", "g"] };
const glyphs = { K: "♚", Q: "♛", R: "♜", B: "♝", N: "♞", P: "♟" };
const state = {
  position: null, selected: null, bestMove: null, variations: [], history: [],
  requestId: 0, analyzing: false, lastMove: null, analysisId: null,
  movePending: false, orientation: 0,
  practice: {
    enabled: false, team: "ry", phase: "idle", analysis: null,
    attemptedMove: null,
  },
};

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: options.body ? { "content-type": "application/json" } : undefined,
    ...options,
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error ?? data.result ?? `Request failed (${response.status})`);
  return data;
}

function setStatus(kind, text) {
  $("#statusDot").className = `status-dot ${kind}`;
  $("#statusText").textContent = text;
}

function isPracticeTurn(position = state.position) {
  return Boolean(state.practice.enabled && position &&
    practiceTeams[state.practice.team]?.includes(position.turn));
}

function updateActionControls() {
  const practicing = isPracticeTurn();
  const reviewing = state.practice.enabled && state.practice.phase === "review";
  $("#analyzeButton").disabled = state.analyzing || practicing || reviewing;
  $("#playBestButton").disabled = reviewing || !state.bestMove;
  $("#practiceNextButton").hidden = !reviewing;
  $("#practiceControls").classList.toggle("active", state.practice.enabled);
  $("#practiceTeam").disabled = !state.practice.enabled;
}

function clearAnalysisDisplay(message, meta = "Moves hidden") {
  state.bestMove = null;
  state.variations = [];
  analysisList.innerHTML = `<div class="empty-state"><span class="empty-icon">?</span><p>${message}</p></div>`;
  $("#analysisMeta").textContent = meta;
  drawArrow(null);
  updateActionControls();
}

let toastTimer;
function toast(message) {
  const element = $("#toast");
  element.textContent = message;
  element.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => element.classList.remove("show"), 2300);
}

function boardPart(fen) {
  const marker = fen.lastIndexOf("-x,");
  return marker >= 0 ? fen.slice(marker + 1) : fen.split("-").at(-1);
}

function parseBoard(fen) {
  const ranks = boardPart(fen).split("/");
  return ranks.map((rank) => {
    const cells = [];
    for (const token of rank.split(",")) {
      if (/^\d+$/.test(token)) cells.push(...Array(Number(token)).fill(null));
      else cells.push(token === "x" ? "x" : token);
    }
    return cells;
  });
}

function splitMove(move) {
  const match = move?.match(/^([a-n](?:1[0-4]|[1-9]))([a-n](?:1[0-4]|[1-9]))([QRBN])?$/);
  return match ? { from: match[1], to: match[2], promotion: match[3] } : null;
}

function squareName(fileIndex, rowIndex) {
  return `${String.fromCharCode(97 + fileIndex)}${14 - rowIndex}`;
}

function viewToBoard(row, file) {
  switch (state.orientation) {
    case 1: return { row: 13 - file, file: row };
    case 2: return { row: 13 - row, file: 13 - file };
    case 3: return { row: file, file: 13 - row };
    default: return { row, file };
  }
}

function boardToView(row, file) {
  switch (state.orientation) {
    case 1: return { row: file, file: 13 - row };
    case 2: return { row: 13 - row, file: 13 - file };
    case 3: return { row: 13 - file, file: row };
    default: return { row, file };
  }
}

function renderPerspective() {
  const positions = ["top", "right", "bottom", "left"];
  const seatedColors = ["y", "g", "r", "b"];
  const displayed = Array(4);
  seatedColors.forEach((color, index) => {
    displayed[(index + state.orientation) % 4] = color;
  });
  positions.forEach((position, index) => {
    const color = displayed[index];
    const tag = $(`#${position}Player`);
    tag.className = `player-tag ${position} ${color}`;
    tag.innerHTML = `<span>${colors[color]}</span><small>${teams[color]}</small>`;
  });
  const bottomColor = displayed[2];
  if (state.practice.enabled && state.practice.phase === "review") {
    $("#boardHint").textContent = "Review · the green arrow shows Athena's preferred move";
  } else if (isPracticeTurn()) {
    $("#boardHint").textContent = `Practice as ${practiceTeams[state.practice.team].map((color) => colors[color]).join(" + ")} · choose without hints`;
  } else {
    $("#boardHint").textContent = `${colors[bottomColor]} perspective · Click a piece, then a highlighted square`;
  }
  $("#rotateBoardButton").setAttribute(
    "aria-label", `Rotate board 90 degrees clockwise; ${colors[displayed[1]]} will be at the bottom`);
}

function legalFrom(square) {
  return (state.position?.legalMoves ?? []).filter((move) => splitMove(move)?.from === square);
}

function renderBoard() {
  if (!state.position) return;
  const cells = parseBoard(state.position.fen);
  boardElement.replaceChildren();
  for (let viewRow = 0; viewRow < 14; viewRow += 1) {
    for (let viewFile = 0; viewFile < 14; viewFile += 1) {
      const { row, file } = viewToBoard(viewRow, viewFile);
      const name = squareName(file, row);
      const value = cells[row]?.[file] ?? null;
      const square = document.createElement("div");
      square.className = `square ${(row + file) % 2 ? "dark" : "light"}`;
      square.dataset.square = name;
      square.dataset.piece = value ?? "";
      square.setAttribute("aria-label", name);
      if (value === "x") square.classList.add("stone");
      if (state.selected === name) square.classList.add("selected");
      if (state.lastMove?.from === name) square.classList.add("last-from");
      if (state.lastMove?.to === name) square.classList.add("last-to");

      const targets = state.selected ? legalFrom(state.selected).map((move) => splitMove(move).to) : [];
      if (targets.includes(name)) square.classList.add("legal-target");
      if (value && value !== "x") {
        square.classList.add("occupied");
        const piece = document.createElement("span");
        piece.className = `piece ${value[0]}`;
        piece.textContent = glyphs[value[1]] ?? value[1];
        piece.setAttribute("aria-label", `${colors[value[0]]} ${value[1]} on ${name}`);
        square.append(piece);
      }
      if (viewFile === 0 || value !== "x" && viewFile === 3) {
        const sideCoordinate = document.createElement("span");
        sideCoordinate.className = "coord rank";
        sideCoordinate.textContent = state.orientation % 2 ? name[0] : name.match(/\d+$/)[0];
        square.append(sideCoordinate);
      }
      if (viewRow === 13 || value !== "x" && viewRow === 10) {
        const bottomCoordinate = document.createElement("span");
        bottomCoordinate.className = "coord file";
        bottomCoordinate.textContent = state.orientation % 2 ? name.match(/\d+$/)[0] : name[0];
        square.append(bottomCoordinate);
      }
      boardElement.append(square);
    }
  }
  renderPerspective();
  drawArrow(state.bestMove);
}

boardElement.addEventListener("pointerdown", (event) => {
  if (event.button !== 0) return;
  const square = event.target.closest(".square");
  if (!square || !boardElement.contains(square)) return;
  event.preventDefault();
  handleSquareClick(square.dataset.square, square.dataset.piece || null);
});

function handleSquareClick(square, value) {
  if (state.movePending) return;
  if (state.practice.enabled && state.practice.phase === "review") {
    toast("Continue to opponent analysis before making the next move");
    return;
  }
  if (state.selected) {
    if (chooseMove(state.selected, square)) return;
    state.selected = null;
  }
  if (value && value !== "x" && legalFrom(square).length) {
    cancelAnalysisForBoardInput();
    state.selected = square;
  }
  renderBoard();
}

function cancelAnalysisForBoardInput() {
  if (!state.analyzing) return;
  // Practice analysis deliberately continues in the background while the
  // player considers and enters a move; none of its output is rendered.
  if (isPracticeTurn() && state.practice.phase === "thinking") return;
  ++state.requestId;
  state.analyzing = false;
  state.analysisId = null;
  $("#analyzeButton").disabled = false;
  $("#searchProgress").classList.remove("active");
  setStatus("ready", "Choose a destination");
  api("/api/stop", { method: "POST" }).catch((error) => toast(error.message));
}

function chooseMove(from, to) {
  const choices = (state.position?.legalMoves ?? []).filter((move) => {
    const parts = splitMove(move); return parts?.from === from && parts.to === to;
  });
  if (!choices.length) return false;
  let selected = choices[0];
  if (choices.length > 1) {
    const promotion = (window.prompt("Promote to Q, R, B, or N", "Q") ?? "Q").toUpperCase();
    selected = choices.find((move) => splitMove(move).promotion === promotion) ?? choices[0];
  }
  state.selected = null;
  renderOptimisticMove(selected);
  playMove(selected, { userMove: true });
  return true;
}

function renderOptimisticMove(move) {
  const parts = splitMove(move);
  if (!parts) return;
  const source = boardElement.querySelector(`[data-square="${parts.from}"]`);
  const target = boardElement.querySelector(`[data-square="${parts.to}"]`);
  const piece = source?.querySelector(".piece");
  if (!source || !target || !piece) return;
  target.querySelector(".piece")?.remove();
  target.append(piece);
  source.classList.remove("occupied", "selected");
  target.classList.add("occupied", "last-to");
  source.classList.add("last-from");
  boardElement.querySelectorAll(".legal-target").forEach((square) => square.classList.remove("legal-target"));
  state.lastMove = parts;
  drawArrow(null);
}

function drawArrow(move) {
  const parts = splitMove(move);
  if (!parts) { arrow.style.display = "none"; return; }
  const point = (square) => {
    const match = square.match(/^([a-n])(\d+)$/);
    const file = match[1].charCodeAt(0) - 97;
    const row = 14 - Number(match[2]);
    const view = boardToView(row, file);
    return { x: view.file + .5, y: view.row + .5 };
  };
  const from = point(parts.from); const to = point(parts.to);
  const dx = to.x - from.x; const dy = to.y - from.y;
  const distance = Math.hypot(dx, dy) || 1;
  const inset = Math.min(.38, distance * .16);
  arrow.setAttribute("x1", from.x + dx / distance * inset);
  arrow.setAttribute("y1", from.y + dy / distance * inset);
  arrow.setAttribute("x2", to.x - dx / distance * .46);
  arrow.setAttribute("y2", to.y - dy / distance * .46);
  arrow.style.display = "block";
}

function updatePosition(position) {
  state.position = position;
  state.selected = null;
  const color = colors[position.turn] ?? "Unknown";
  const badge = $("#turnBadge");
  badge.textContent = `${color} to move`;
  badge.className = `turn-badge ${position.turn}`;
  $("#setup").value = position.setup;
  renderBoard();
}

function formatScore(line) {
  if (line.scoreType === "mate") return `${line.score < 0 ? "−" : ""}M${Math.abs(line.score)}`;
  const pawns = line.score / 100;
  return `${pawns > 0 ? "+" : ""}${pawns.toFixed(2)}`;
}

function renderAnalysis(result) {
  $("#analysisEyebrow").textContent = "Ranked choices";
  $("#analysisTitle").textContent = "Candidate moves";
  state.variations = result.variations;
  state.bestMove = result.variations[0]?.move ?? null;
  analysisList.replaceChildren();
  if (!result.variations.length) {
    analysisList.innerHTML = '<div class="empty-state"><span class="empty-icon">—</span><p>No legal moves or the search was stopped before producing a line.</p></div>';
  }
  for (const line of result.variations) {
    const row = document.createElement("div");
    row.className = `analysis-row ${line.rank === 1 ? "best" : ""}`;
    const scoreClass = line.scoreType === "mate" ? "mate" : line.score > 0 ? "positive" : line.score < 0 ? "negative" : "";
    row.innerHTML = `<span class="rank">${line.rank}</span><span class="move-name">${line.move}</span><span class="evaluation ${scoreClass}">${formatScore(line)}</span><span class="pv" title="${line.pv.join(" ")}">${line.pv.join(" ")}<small>d${line.depth}</small></span>`;
    row.addEventListener("mouseenter", () => drawArrow(line.move));
    row.addEventListener("mouseleave", () => drawArrow(state.bestMove));
    row.addEventListener("click", () => { drawArrow(line.move); document.querySelectorAll(".analysis-row").forEach((item) => item.classList.toggle("preview", item === row)); });
    row.addEventListener("dblclick", () => playMove(line.move, { userMove: false }));
    analysisList.append(row);
  }
  const meta = result.stats;
  $("#analysisMeta").textContent = meta.nodes === undefined ? "Waiting for results" : `${meta.nodes.toLocaleString()} nodes · ${meta.timeMs} ms · sd ${meta.selectiveDepth}`;
  drawArrow(state.bestMove);
  updateActionControls();
}

function renderPracticeReview(result, attemptedMove) {
  const allLines = result?.variations ?? [];
  const best = allLines[0] ?? null;
  const attempted = allLines.find((line) => line.move === attemptedMove) ?? null;
  const visibleCount = Number($("#multipv").value);
  const lines = allLines.slice(0, visibleCount);
  if (attempted && !lines.some((line) => line.move === attempted.move)) lines.push(attempted);

  state.variations = lines;
  state.bestMove = best?.move ?? null;
  $("#analysisEyebrow").textContent = "Practice review";
  $("#analysisTitle").textContent = "Your move compared";
  analysisList.replaceChildren();

  if (!lines.length) {
    analysisList.innerHTML = '<div class="empty-state"><span class="empty-icon">—</span><p>The search stopped before producing a comparison. Try a longer search time or make the move again.</p></div>';
  }
  for (const line of lines) {
    const row = document.createElement("div");
    const played = line.move === attemptedMove;
    row.className = `analysis-row ${line.rank === 1 ? "best" : ""} ${played ? "played" : ""}`;
    const scoreClass = line.scoreType === "mate" ? "mate" : line.score > 0 ? "positive" : line.score < 0 ? "negative" : "";
    row.innerHTML = `<span class="rank">${line.rank}</span><span class="move-name">${line.move}</span><span class="evaluation ${scoreClass}">${formatScore(line)}</span><span class="pv" title="${line.pv.join(" ")}">${line.pv.join(" ")}<small>d${line.depth}</small></span>`;
    row.addEventListener("mouseenter", () => drawArrow(line.move));
    row.addEventListener("mouseleave", () => drawArrow(state.bestMove));
    row.addEventListener("click", () => { drawArrow(line.move); document.querySelectorAll(".analysis-row").forEach((item) => item.classList.toggle("preview", item === row)); });
    analysisList.append(row);
  }

  if (!attempted) {
    $("#analysisMeta").textContent = `${attemptedMove} · search stopped before this move received a score`;
  } else if (best?.scoreType === "cp" && attempted.scoreType === "cp") {
    const cost = Math.max(0, best.score - attempted.score) / 100;
    $("#analysisMeta").textContent = `Your move ${formatScore(attempted)} · best ${formatScore(best)} · cost ${cost.toFixed(2)}`;
  } else {
    $("#analysisMeta").textContent = `Your move ${formatScore(attempted)} · best ${best ? formatScore(best) : "—"}`;
  }

  $("#searchProgress").classList.remove("active");
  drawArrow(state.bestMove);
  updateActionControls();
}

function renderProgress(result, active = true) {
  const progress = result.progress ?? {};
  const total = progress.total ?? 0;
  const completed = progress.completed ?? 0;
  const percent = total ? Math.min(100, Math.round(completed / total * 100)) : 0;
  $("#searchProgress").classList.toggle("active", active);
  $("#progressFill").style.width = `${percent}%`;
  $("#progressPercent").textContent = `${percent}%`;
  $("#progressPhase").textContent = !active
    ? progress.iterationComplete ? `Depth ${progress.depth} complete` : `Stopped at depth ${progress.depth}`
    : progress.depth
      ? `Depth ${progress.depth} · ${completed}/${total} root moves complete`
      : "Preparing depth 1";
  const nodes = result.stats?.nodes ?? 0;
  const time = result.stats?.timeMs ?? 0;
  $("#progressDetail").textContent = `${nodes.toLocaleString()} nodes · ${time} ms · current-depth root progress`;
}

function renderHistory() {
  historyElement.replaceChildren();
  if (!state.history.length) {
    historyElement.innerHTML = '<span class="muted">No moves played</span>';
    return;
  }
  state.history.forEach((entry, index) => {
    const item = document.createElement("span");
    item.className = "history-ply";
    item.innerHTML = `<em>${index + 1}.</em>${entry}`;
    historyElement.append(item);
  });
}

async function analyzePosition({ playResponse = false, hidden = false, maxLines = null } = {}) {
  const requestId = ++state.requestId;
  state.analyzing = true;
  setStatus("busy", hidden ? "Thinking privately" : "Searching");
  updateActionControls();
  try {
    const started = await api("/api/analyze", {
      method: "POST",
      body: JSON.stringify({
        depth: Number($("#depth").value),
        moveTime: Number($("#moveTime").value),
        multipv: maxLines ?? Number($("#multipv").value),
      }),
    });
    state.analysisId = started.id;
    let lastSequence = -1;
    let result;
    while (requestId === state.requestId) {
      result = await api(`/api/analysis?id=${started.id}`);
      if (result.sequence !== lastSequence) {
        lastSequence = result.sequence;
        if (hidden) {
          if (result.variations.length) state.practice.analysis = result;
        } else if (result.variations.length) renderAnalysis(result);
        renderProgress(result, result.status !== "complete");
      }
      if (result.status === "complete") break;
      if (result.status === "error") throw new Error(result.error ?? "Analysis failed");
      await delay(120);
    }
    if (requestId !== state.requestId) return;
    if (hidden) {
      if (result?.variations.length) state.practice.analysis = result;
      setStatus("ready", "Your move");
      return result;
    }
    if (playResponse && $("#autoReply").checked && result?.variations[0]) {
      await playMove(result.variations[0].move, { userMove: false, analyzeAfter: true });
      return;
    }
    setStatus("ready", "Ready");
  } catch (error) {
    if (requestId === state.requestId) { setStatus("error", "Search error"); toast(error.message); }
  } finally {
    if (requestId === state.requestId) { state.analyzing = false; updateActionControls(); }
  }
}

async function finishPracticeAnalysis() {
  const analysisId = state.analysisId;
  if (!analysisId) return state.practice.analysis;

  await api("/api/stop", { method: "POST" });
  let result = state.practice.analysis;
  for (let attempt = 0; attempt < 40; attempt += 1) {
    const snapshot = await api(`/api/analysis?id=${analysisId}`);
    if (snapshot.variations.length) result = snapshot;
    if (snapshot.status === "complete" || snapshot.status === "error") break;
    await delay(25);
  }
  ++state.requestId;
  state.analyzing = false;
  state.analysisId = null;
  state.practice.analysis = result;
  return result;
}

function beginPracticeTurn() {
  state.practice.phase = "thinking";
  state.practice.analysis = null;
  state.practice.attemptedMove = null;
  $("#analysisEyebrow").textContent = "Practice mode";
  $("#analysisTitle").textContent = "Find the best move";
  $("#practiceNote").textContent = "Athena is analyzing privately. Commit a move to reveal the comparison.";
  clearAnalysisDisplay("Candidate moves and arrows are hidden. Choose the strongest move you can find.", "Private analysis");
  renderBoard();
  void analyzePosition({
    hidden: true,
    maxLines: Math.max(1, state.position?.legalMoves?.length ?? 1),
  });
}

async function continueFromPracticeReview() {
  if (state.practice.phase !== "review") return;
  state.practice.phase = "opponent";
  state.practice.analysis = null;
  state.practice.attemptedMove = null;
  $("#practiceNote").textContent = "Opponent turn: Athena's candidates and best-move arrow are visible normally.";
  clearAnalysisDisplay("Analyzing the opponent's strongest continuations.", "Starting opponent analysis");
  renderBoard();
  await analyzePosition();
}

async function routeAnalysisForPosition({ force = false } = {}) {
  if (state.practice.enabled && isPracticeTurn()) {
    beginPracticeTurn();
    return;
  }
  state.practice.phase = state.practice.enabled ? "opponent" : "idle";
  if (state.practice.enabled) {
    $("#practiceNote").textContent = "Opponent turn: Athena's candidates and best-move arrow are visible normally.";
  }
  renderBoard();
  updateActionControls();
  if (force || $("#autoAnalyze").checked) await analyzePosition();
  else setStatus("ready", "Ready");
}

async function stopAnalysis() {
  ++state.requestId;
  await api("/api/stop", { method: "POST" });
  state.analyzing = false;
  state.analysisId = null;
  $("#searchProgress").classList.remove("active");
  setStatus("ready", "Stopped");
  updateActionControls();
}

async function playMove(move, { userMove = false, analyzeAfter = true } = {}) {
  if (!move || state.movePending) return;
  state.movePending = true;
  const practiceAttempt = userMove && isPracticeTurn() && state.practice.phase === "thinking";
  let practiceResult = null;
  if (practiceAttempt) {
    setStatus("busy", "Grading your move");
    try {
      practiceResult = await finishPracticeAnalysis();
    } catch (error) {
      toast(`Practice analysis stopped early: ${error.message}`);
      ++state.requestId;
      state.analyzing = false;
    }
  }
  ++state.requestId;
  state.analyzing = false;
  state.analysisId = null;
  updateActionControls();
  setStatus("busy", "Applying move");
  try {
    const position = await api("/api/move", { method: "POST", body: JSON.stringify({ move }) });
    state.history.push(move);
    state.lastMove = splitMove(move);
    state.bestMove = null;
    state.variations = [];
    renderHistory();
    updatePosition(position);
    state.movePending = false;
    if (practiceAttempt) {
      state.practice.phase = "review";
      state.practice.analysis = practiceResult;
      state.practice.attemptedMove = move;
      $("#practiceNote").textContent = "Review your move, then continue to visible opponent analysis.";
      renderPracticeReview(practiceResult, move);
      renderBoard();
      setStatus("ready", "Review your move");
    } else if (state.practice.enabled && isPracticeTurn(position)) {
      beginPracticeTurn();
    } else if (analyzeAfter && $("#autoAnalyze").checked) {
      await analyzePosition({ playResponse: userMove });
    } else setStatus("ready", "Ready");
  } catch (error) {
    setStatus("error", "Move rejected"); toast(error.message);
    if (state.position) renderBoard();
  } finally {
    state.movePending = false;
  }
}

async function resetBoard() {
  ++state.requestId;
  state.analysisId = null;
  setStatus("busy", "Resetting");
  const position = await api("/api/reset", { method: "POST", body: JSON.stringify({ setup: $("#setup").value }) });
  state.history = []; state.lastMove = null; state.bestMove = null; state.variations = [];
  state.practice.phase = "idle";
  state.practice.analysis = null; state.practice.attemptedMove = null;
  renderHistory(); renderAnalysis({ variations: [], stats: {} }); updatePosition(position);
  await routeAnalysisForPosition();
}

async function undoMove() {
  ++state.requestId;
  state.analysisId = null;
  setStatus("busy", "Undoing");
  const position = await api("/api/undo", { method: "POST" });
  if (position.result === "undo empty") return toast("There is nothing to undo");
  state.history.pop(); state.lastMove = null; state.bestMove = null;
  state.practice.phase = "idle";
  state.practice.analysis = null; state.practice.attemptedMove = null;
  renderHistory(); updatePosition(position);
  await routeAnalysisForPosition();
}

$("#depth").addEventListener("input", (event) => $("#depthValue").textContent = event.target.value);
function updateTimeLabel() {
  const unlimited = Number($("#moveTime").value) === 0;
  $("#timeValue").textContent = unlimited ? "Unlimited" : `${$("#moveTime").value} ms`;
  $("#depth").disabled = unlimited;
  $("#depthValue").textContent = unlimited ? "∞" : $("#depth").value;
}

async function refreshPracticeMode() {
  ++state.requestId;
  if (state.analyzing) await api("/api/stop", { method: "POST" });
  state.analyzing = false;
  state.analysisId = null;
  state.practice.enabled = $("#practiceMode").checked;
  state.practice.team = $("#practiceTeam").value;
  state.practice.phase = "idle";
  state.practice.analysis = null;
  state.practice.attemptedMove = null;
  state.bestMove = null;
  state.variations = [];
  $("#practiceNote").textContent = "Your team's candidate moves stay hidden until after you commit a move.";
  clearAnalysisDisplay(
    state.practice.enabled ? "Preparing practice mode." : "Run an analysis to compare the strongest moves.",
    state.practice.enabled ? "Practice mode" : "No analysis yet",
  );
  await routeAnalysisForPosition();
}

$("#moveTime").addEventListener("input", updateTimeLabel);
$("#analyzeButton").addEventListener("click", () => analyzePosition());
$("#stopButton").addEventListener("click", stopAnalysis);
$("#undoButton").addEventListener("click", undoMove);
$("#resetButton").addEventListener("click", resetBoard);
$("#rotateBoardButton").addEventListener("click", () => {
  state.orientation = (state.orientation + 1) % 4;
  renderBoard();
});
$("#playBestButton").addEventListener("click", () => playMove(state.bestMove, { userMove: false }));
$("#practiceNextButton").addEventListener("click", continueFromPracticeReview);
$("#practiceMode").addEventListener("change", refreshPracticeMode);
$("#practiceTeam").addEventListener("change", refreshPracticeMode);
$("#setup").addEventListener("change", resetBoard);
$("#hash").addEventListener("change", async (event) => {
  await api("/api/options", { method: "POST", body: JSON.stringify({ hash: Number(event.target.value) }) });
  toast(`Hash resized to ${event.target.value} MiB`);
});
$("#threads").addEventListener("change", async (event) => {
  await api("/api/options", { method: "POST", body: JSON.stringify({ threads: Number(event.target.value) }) });
  toast(`Analysis will use ${event.target.value} thread${event.target.value === "1" ? "" : "s"}`);
});
window.addEventListener("keydown", (event) => {
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "z") { event.preventDefault(); undoMove(); }
  if (event.key === "Escape") { state.selected = null; renderBoard(); }
});

updateTimeLabel();
try {
  updatePosition(await api("/api/state"));
  setStatus("ready", "Ready");
  renderHistory();
  updateActionControls();
  await routeAnalysisForPosition();
} catch (error) {
  setStatus("error", "Engine unavailable");
  toast(error.message);
}
