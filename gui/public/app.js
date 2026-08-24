const $ = (selector) => document.querySelector(selector);
const boardElement = $("#board");
const arrow = $("#bestMoveArrow");
const analysisList = $("#analysisList");
const historyElement = $("#moveHistory");

const colors = { r: "Red", b: "Blue", y: "Yellow", g: "Green" };
const glyphs = { K: "♚", Q: "♛", R: "♜", B: "♝", N: "♞", P: "♟" };
const state = {
  position: null, selected: null, bestMove: null, variations: [], history: [],
  requestId: 0, analyzing: false, lastMove: null, analysisId: null,
  movePending: false,
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

function legalFrom(square) {
  return (state.position?.legalMoves ?? []).filter((move) => splitMove(move)?.from === square);
}

function renderBoard() {
  if (!state.position) return;
  const cells = parseBoard(state.position.fen);
  boardElement.replaceChildren();
  for (let row = 0; row < 14; row += 1) {
    for (let file = 0; file < 14; file += 1) {
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
      if (file === 0 || value !== "x" && file === 3) {
        const rank = document.createElement("span"); rank.className = "coord rank"; rank.textContent = 14 - row; square.append(rank);
      }
      if (row === 13 || value !== "x" && row === 10) {
        const coord = document.createElement("span"); coord.className = "coord file"; coord.textContent = String.fromCharCode(97 + file); square.append(coord);
      }
      boardElement.append(square);
    }
  }
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
    return { x: match[1].charCodeAt(0) - 96 - .5, y: 14 - Number(match[2]) + .5 };
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
  state.variations = result.variations;
  state.bestMove = result.variations[0]?.move ?? null;
  $("#playBestButton").disabled = !state.bestMove;
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

async function analyzePosition({ playResponse = false } = {}) {
  const requestId = ++state.requestId;
  state.analyzing = true;
  setStatus("busy", "Searching");
  $("#analyzeButton").disabled = true;
  try {
    const started = await api("/api/analyze", {
      method: "POST",
      body: JSON.stringify({ depth: Number($("#depth").value), moveTime: Number($("#moveTime").value), multipv: Number($("#multipv").value) }),
    });
    state.analysisId = started.id;
    let lastSequence = -1;
    let result;
    while (requestId === state.requestId) {
      result = await api(`/api/analysis?id=${started.id}`);
      if (result.sequence !== lastSequence) {
        lastSequence = result.sequence;
        if (result.variations.length) renderAnalysis(result);
        renderProgress(result, result.status !== "complete");
      }
      if (result.status === "complete") break;
      if (result.status === "error") throw new Error(result.error ?? "Analysis failed");
      await delay(120);
    }
    if (requestId !== state.requestId) return;
    if (playResponse && $("#autoReply").checked && result?.variations[0]) {
      await playMove(result.variations[0].move, { userMove: false, analyzeAfter: true });
      return;
    }
    setStatus("ready", "Ready");
  } catch (error) {
    if (requestId === state.requestId) { setStatus("error", "Search error"); toast(error.message); }
  } finally {
    if (requestId === state.requestId) { state.analyzing = false; $("#analyzeButton").disabled = false; }
  }
}

async function stopAnalysis() {
  ++state.requestId;
  await api("/api/stop", { method: "POST" });
  state.analyzing = false;
  state.analysisId = null;
  $("#analyzeButton").disabled = false;
  $("#searchProgress").classList.remove("active");
  setStatus("ready", "Stopped");
}

async function playMove(move, { userMove = false, analyzeAfter = true } = {}) {
  if (!move || state.movePending) return;
  state.movePending = true;
  ++state.requestId;
  state.analyzing = false;
  state.analysisId = null;
  $("#analyzeButton").disabled = false;
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
    if (analyzeAfter && $("#autoAnalyze").checked) {
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
  $("#analyzeButton").disabled = false;
  setStatus("busy", "Resetting");
  const position = await api("/api/reset", { method: "POST", body: JSON.stringify({ setup: $("#setup").value }) });
  state.history = []; state.lastMove = null; state.bestMove = null; state.variations = [];
  renderHistory(); renderAnalysis({ variations: [], stats: {} }); updatePosition(position);
  if ($("#autoAnalyze").checked) analyzePosition(); else setStatus("ready", "Ready");
}

async function undoMove() {
  ++state.requestId;
  state.analysisId = null;
  $("#analyzeButton").disabled = false;
  setStatus("busy", "Undoing");
  const position = await api("/api/undo", { method: "POST" });
  if (position.result === "undo empty") return toast("There is nothing to undo");
  state.history.pop(); state.lastMove = null; state.bestMove = null;
  renderHistory(); updatePosition(position);
  if ($("#autoAnalyze").checked) analyzePosition(); else setStatus("ready", "Ready");
}

$("#depth").addEventListener("input", (event) => $("#depthValue").textContent = event.target.value);
function updateTimeLabel() {
  const unlimited = Number($("#moveTime").value) === 0;
  $("#timeValue").textContent = unlimited ? "Unlimited" : `${$("#moveTime").value} ms`;
  $("#depth").disabled = unlimited;
  $("#depthValue").textContent = unlimited ? "∞" : $("#depth").value;
}
$("#moveTime").addEventListener("input", updateTimeLabel);
$("#analyzeButton").addEventListener("click", () => analyzePosition());
$("#stopButton").addEventListener("click", stopAnalysis);
$("#undoButton").addEventListener("click", undoMove);
$("#resetButton").addEventListener("click", resetBoard);
$("#playBestButton").addEventListener("click", () => playMove(state.bestMove, { userMove: false }));
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
  if ($("#autoAnalyze").checked) analyzePosition();
} catch (error) {
  setStatus("error", "Engine unavailable");
  toast(error.message);
}
