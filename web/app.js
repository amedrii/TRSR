const config = {
  apiBase: window.TRSR_API_URL || "https://api.trsr.app",
};

const githubReleaseApi = "https://api.github.com/repos/amedrii/TRSR/releases/latest";

const leaderboardFilters = {
  game: document.getElementById("game-filter"),
  level: document.getElementById("level-filter"),
  category: document.getElementById("category-filter"),
  subcategory: document.getElementById("subcategory-filter"),
};

let leaderboardRequest = 0;

function endpoint(path) {
  return `${config.apiBase.replace(/\/$/, "")}${path}`;
}

async function loadLatestReleaseDownload() {
  const links = [...document.querySelectorAll("[data-release-download]")];
  if (!links.length) return;

  links.forEach((link) => link.addEventListener("click", (event) => {
    if (link.getAttribute("aria-disabled") === "true") event.preventDefault();
  }));

  try {
    const response = await fetch(githubReleaseApi, {
      headers: { Accept: "application/vnd.github+json" },
    });
    if (!response.ok) throw new Error(`GitHub release request failed (${response.status})`);
    const release = await response.json();
    const asset = (release.assets || []).find((item) => /^TRSR-[0-9][^/]*\.zip$/i.test(item.name));
    if (!asset?.browser_download_url) throw new Error("Latest release has no TRSR ZIP asset");

    links.forEach((link) => {
      link.href = asset.browser_download_url;
      link.target = "_blank";
      link.rel = "noreferrer";
      link.removeAttribute("aria-disabled");
    });
    document.getElementById("download-link").innerHTML = `Download ${escapeHtml(asset.name)} <span>↓</span>`;
  } catch {
    const downloadLink = document.getElementById("download-link");
    downloadLink.textContent = "Latest release is unavailable";
    downloadLink.title = "Publish a GitHub release with a TRSR-x.x.x.zip asset, then reload this page.";
  }
}

async function fetchJson(path, options = {}) {
  const { headers: extraHeaders = {}, ...rest } = options;
  const response = await fetch(endpoint(path), {
    credentials: "include",
    ...rest,
    headers: { Accept: "application/json", ...extraHeaders },
  });
  const result = await response.json().catch(() => null);
  if (!response.ok) throw new Error(result?.error || `Request failed (${response.status})`);
  return result;
}

function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "'": "&#039;",
    '"': "&quot;",
  })[character]);
}

function setText(id, value) {
  document.getElementById(id).textContent = value;
}

function setOverview(overview) {
  setText("online-count", overview.online ?? "—");
  setText("queue-count", overview.queue ?? "—");
  setText("race-count", overview.racing ?? "—");
  setText("data-mode", "LIVE");
}

function setOverviewUnavailable() {
  for (const id of ["online-count", "queue-count", "race-count"]) setText(id, "—");
  setText("data-mode", "UNAVAILABLE");
}

function renderPlayers(players) {
  const list = document.getElementById("player-list");
  setText("presence-updated", "Live now");
  if (!players.length) {
    list.innerHTML = '<li class="loading-player">No players are currently shown online or in queue.</li>';
    return;
  }
  list.innerHTML = players.slice(0, 8).map((player) => {
    const state = String(player.state || "online").toLowerCase();
    const stateClass = state.includes("queue") || state === "ready"
      ? "queue" : state.includes("race") || state === "running" ? "racing" : "";
    return `<li><span><span class="player-name">${escapeHtml(player.name || "Player")}</span><span class="player-game">${escapeHtml(player.game || "Tomb Raider: Anniversary")}</span></span><span class="state ${stateClass}">${escapeHtml(state)}</span></li>`;
  }).join("");
}

function setPlayersUnavailable() {
  setText("presence-updated", "Unavailable");
  document.getElementById("player-list").innerHTML = '<li class="loading-player">Player activity is not available yet.</li>';
}

function currentFilters() {
  return Object.fromEntries(Object.entries(leaderboardFilters).map(([key, element]) => [key, element.value]));
}

function leaderboardPath(filters) {
  const params = new URLSearchParams({ game: filters.game });
  for (const key of ["level", "category", "subcategory"]) {
    if (filters[key] !== "all") params.set(key, filters[key]);
  }
  return `/public/leaderboards?${params}`;
}

function rowMatchesFilters(entry, filters) {
  return (filters.level === "all" || entry.level === filters.level)
    && (filters.category === "all" || entry.category === filters.category)
    && (filters.subcategory === "all" || entry.subcategory === filters.subcategory);
}

function renderLeaderboard(entries, filters) {
  const body = document.getElementById("leaderboard-body");
  const visible = entries.filter((entry) => rowMatchesFilters(entry, filters));
  if (!visible.length) {
    body.innerHTML = '<tr class="empty-row"><td colspan="7">No match results for these filters yet.</td></tr>';
    return;
  }
  body.innerHTML = visible.map((entry, index) => `<tr>
    <td>${Number.isInteger(entry.rank) ? entry.rank : index + 1}</td>
    <td><span class="runner">${escapeHtml(entry.playerName || entry.player || "Player")}</span></td>
    <td>${escapeHtml(entry.level || "—")}</td>
    <td>${escapeHtml(entry.category || "—")}</td>
    <td>${escapeHtml(entry.subcategory || "—")}</td>
    <td><span class="time">${escapeHtml(entry.time || "—")}</span></td>
    <td>${Number.isInteger(entry.wins) ? entry.wins : "—"}</td>
  </tr>`).join("");
}

async function loadLeaderboard() {
  const requestNumber = ++leaderboardRequest;
  const filters = currentFilters();
  const body = document.getElementById("leaderboard-body");
  body.innerHTML = '<tr class="loading-row"><td colspan="7">Loading results…</td></tr>';
  try {
    const response = await fetchJson(leaderboardPath(filters));
    if (requestNumber !== leaderboardRequest) return;
    if (!Array.isArray(response.entries)) throw new Error("Invalid leaderboard response");
    renderLeaderboard(response.entries, filters);
  } catch {
    if (requestNumber === leaderboardRequest) {
      body.innerHTML = '<tr class="empty-row"><td colspan="7">Leaderboards are not available yet.</td></tr>';
    }
  }
}

async function loadActivity() {
  const [overview, players] = await Promise.allSettled([
    fetchJson("/public/overview"),
    fetchJson("/public/players"),
  ]);
  if (overview.status === "fulfilled") setOverview(overview.value);
  else setOverviewUnavailable();
  if (players.status === "fulfilled" && Array.isArray(players.value.players)) {
    renderPlayers(players.value.players);
  } else {
    setPlayersUnavailable();
  }
}

function setupNavigation() {
  const toggle = document.querySelector(".nav-toggle");
  const nav = document.querySelector(".site-nav");
  toggle.addEventListener("click", () => {
    const isOpen = nav.classList.toggle("open");
    toggle.setAttribute("aria-expanded", String(isOpen));
  });
  nav.querySelectorAll("a").forEach((link) => link.addEventListener("click", () => {
    nav.classList.remove("open");
    toggle.setAttribute("aria-expanded", "false");
  }));
  const updateActive = () => {
    const target = window.location.hash || "#home";
    nav.querySelectorAll("a").forEach((link) => {
      link.classList.toggle("active", link.getAttribute("href") === target);
    });
  };
  window.addEventListener("hashchange", updateActive);
  updateActive();
}

function clearProfile() {
  setText("profile-name", "Not connected");
  setText("profile-state", "Signed out");
  setText("profile-wins", "—");
  setText("profile-best", "—");
  document.querySelector(".account-button").textContent = "Connect Speedrun.com";
  document.getElementById("profile-connect").hidden = false;
  document.getElementById("logout-button").hidden = true;
}

function showProfile(data) {
  const name = data.profile?.displayName || data.profile?.id || "Player";
  setText("profile-name", name);
  setText("profile-state", "Connected");
  setText("profile-wins", data.stats?.winningFinishes ?? 0);
  setText("profile-best", data.stats?.finishes ?? 0);
  document.querySelector(".account-button").textContent = name;
  document.getElementById("profile-connect").hidden = true;
  document.getElementById("logout-button").hidden = false;
}

async function loadProfile() {
  try {
    showProfile(await fetchJson("/auth/me"));
  } catch {
    clearProfile();
  }
}

function setupAccountDialog() {
  const dialog = document.getElementById("login-dialog");
  const form = document.getElementById("login-form");
  const keyInput = document.getElementById("api-key");
  const message = document.getElementById("login-message");
  const submit = form.querySelector('button[type="submit"]');

  document.querySelectorAll("[data-open-login]").forEach((button) => {
    button.addEventListener("click", () => {
      if (button.classList.contains("account-button")
          && document.getElementById("profile-connect").hidden) {
        window.location.hash = "#profile";
        return;
      }
      message.textContent = "";
      dialog.showModal();
    });
  });
  document.querySelectorAll("[data-close-login]").forEach((button) => {
    button.addEventListener("click", () => dialog.close());
  });
  dialog.addEventListener("close", () => {
    keyInput.value = "";
    message.textContent = "";
  });

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const apiKey = keyInput.value.trim();
    if (!apiKey) return;
    submit.disabled = true;
    message.textContent = "Connecting…";
    try {
      await fetchJson("/auth/speedrun", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ apiKey }),
      });
      keyInput.value = "";
      dialog.close();
      await loadProfile();
    } catch (error) {
      keyInput.value = "";
      message.textContent = error.message || "Could not connect your account.";
    } finally {
      submit.disabled = false;
    }
  });
  submit.disabled = false;

  document.getElementById("logout-button").addEventListener("click", async () => {
    try {
      await fetchJson("/auth/logout", { method: "POST" });
      clearProfile();
    } catch {
      setText("profile-state", "Could not sign out. Please try again.");
    }
  });
}

setupNavigation();
setupAccountDialog();
loadLatestReleaseDownload();
for (const element of Object.values(leaderboardFilters)) {
  element.addEventListener("change", loadLeaderboard);
}
loadActivity();
loadLeaderboard();
loadProfile();
setInterval(loadActivity, 30_000);
