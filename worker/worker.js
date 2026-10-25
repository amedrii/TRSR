const LEVELS = ["Mountain Caves", "City of Vilcabamba", "The Lost Valley", "Tomb of Qualopec", "St. Francis Folly", "The Coliseum", "Midas Palace", "Tomb of Tihocan", "Temple of Khamoon", "Obelisk of Khamoon", "Sanctuary of the Scion", "Natla's Mines", "The Great Pyramid", "Final Conflict", "Croft Manor"];
const CATEGORIES = ["Any%", "100%"];
const SUBCATEGORIES = ["No Bug Jump", "Bug Jump", "Better Than Glitchless", "Glitchless"];
const headers = { "Access-Control-Allow-Origin": "https://trsr.app", "Access-Control-Allow-Credentials": "true", "Access-Control-Allow-Methods": "GET, POST, OPTIONS", "Access-Control-Allow-Headers": "Content-Type", "Content-Type": "application/json" };
const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers });
function randomItem(items) {
  if (!Array.isArray(items) || items.length === 0) {
    throw new Error("Cannot choose an item from an empty list.");
  }
  // Rejection sampling avoids modulo bias and uses the Worker crypto RNG,
  // rather than the implementation-dependent Math.random().
  const range = 0x100000000;
  const limit = range - (range % items.length);
  let value;
  do {
    value = crypto.getRandomValues(new Uint32Array(1))[0];
  } while (value >= limit);
  return items[value % items.length];
}

// A selected-ruleset queue fixes category/subcategory, then rolls its level
// only after two players are paired. The level selector is not a queue filter.
const queueKey = (type, category, subcategory) =>
  type === "random" ? "random" : `ruleset:${category}:${subcategory}`;
const GO_COUNTDOWN_MS = 5000;
const EARLY_START_TOLERANCE_MS = 750;
const FINISH_GRACE_MS = 5000;
const MINIMUM_FINISH_WINDOW_MS = 10000;
const MAXIMUM_FINISH_WINDOW_MS = 30000;
const MATCH_ACCEPT_TIMEOUT_MS = 20000;
const ADMIN_DISPLAY_NAME = "amedrii";

async function body(request) {
  try { return await request.json(); } catch { return null; }
}

async function hash(value) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function sessionFor(env, token) {
  if (typeof token !== "string" || !token.trim()) return null;
  return env.DB.prepare(`SELECT sessions.speedrun_user_id, profiles.display_name FROM sessions JOIN profiles ON profiles.speedrun_user_id = sessions.speedrun_user_id WHERE sessions.token_hash = ? AND sessions.expires_at > ?`)
    .bind(await hash(token), new Date().toISOString()).first();
}

function speedrunProfileCard(user, fallbackId) {
  return {
    displayName: user?.names?.international ?? fallbackId,
    avatarUrl: user?.assets?.image?.uri ?? user?.assets?.icon?.uri ?? "",
    countryCode: user?.location?.country?.code?.toUpperCase?.() ?? ""
  };
}

async function playerCardFor(env, speedrunUserId) {
  const stored = await env.DB.prepare(
    `SELECT display_name, avatar_url, country_code FROM profiles WHERE speedrun_user_id = ?`
  ).bind(speedrunUserId).first();

  if (stored?.avatar_url) {
    return {
      displayName: stored.display_name || speedrunUserId,
      avatarUrl: stored.avatar_url,
      countryCode: stored.country_code || ""
    };
  }

  // Existing linked accounts predate avatar/country storage. Refresh their
  // public Speedrun.com profile once instead of forcing users to relink.
  try {
    const response = await fetch(
      `https://www.speedrun.com/api/v1/users/${encodeURIComponent(speedrunUserId)}`
    );
    if (!response.ok) throw new Error("Speedrun.com profile lookup failed.");
    const card = speedrunProfileCard((await response.json()).data, speedrunUserId);
    await env.DB.prepare(
      `UPDATE profiles
       SET display_name = ?, avatar_url = ?, country_code = ?
       WHERE speedrun_user_id = ?`
    ).bind(card.displayName, card.avatarUrl || null, card.countryCode || null, speedrunUserId).run();
    return card;
  } catch {
    return {
      displayName: stored?.display_name || speedrunUserId,
      avatarUrl: "",
      countryCode: stored?.country_code || ""
    };
  }
}

async function matchForClient(env, match) {
  const cards = await Promise.all(
    match.players.map((player) => playerCardFor(env, player.speedrunUserId))
  );
  return {
    ...match,
    players: match.players.map((player, index) => ({ ...player, profile: cards[index] }))
  };
}

async function resolveExpiredFinishWindow(env, matchId, nowMilliseconds = Date.now()) {
  const existingOutcome = await env.DB.prepare(
    `SELECT winner_player_id, winner_igt_milliseconds, decided_at_utc
     FROM match_outcomes WHERE match_id = ?`
  ).bind(matchId).first();
  if (existingOutcome) return { outcome: existingOutcome, window: null };

  const window = await env.DB.prepare(
    `SELECT first_finisher_player_id, first_finish_igt_milliseconds,
            first_finished_at_utc, finish_deadline_utc
     FROM match_finish_windows WHERE match_id = ?`
  ).bind(matchId).first();
  if (!window || Date.parse(window.finish_deadline_utc) > nowMilliseconds) {
    return { outcome: null, window };
  }

  const decidedAtUtc = new Date(nowMilliseconds).toISOString();
  await env.DB.prepare(
    `INSERT OR IGNORE INTO match_outcomes
       (match_id, winner_player_id, winner_igt_milliseconds, decided_at_utc)
     VALUES (?, ?, ?, ?)`
  ).bind(
    matchId,
    window.first_finisher_player_id,
    window.first_finish_igt_milliseconds,
    decidedAtUtc
  ).run();

  const outcome = await env.DB.prepare(
    `SELECT winner_player_id, winner_igt_milliseconds, decided_at_utc
     FROM match_outcomes WHERE match_id = ?`
  ).bind(matchId).first();
  return { outcome, window };
}

async function deleteMatchState(env, match) {
  await env.DB.batch([
    env.DB.prepare(`DELETE FROM match_presence WHERE match_id = ?`).bind(match.id),
    env.DB.prepare(`DELETE FROM match_navigation_schedule WHERE match_id = ?`).bind(match.id),
    env.DB.prepare(`DELETE FROM match_start_schedule WHERE match_id = ?`).bind(match.id),
    env.DB.prepare(`DELETE FROM match_race_starts WHERE match_id = ?`).bind(match.id),
    env.DB.prepare(`DELETE FROM match_finish_windows WHERE match_id = ?`).bind(match.id),
    ...match.players.map((player) => env.DB.prepare(`DELETE FROM matches WHERE player_id = ?`).bind(player.playerId))
  ]);
}

function formatIgt(milliseconds) {
  const totalSeconds = Math.floor(milliseconds / 1000);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  const centiseconds = Math.floor((milliseconds % 1000) / 10);
  return (hours ? String(hours).padStart(2, "0") + ":" : "")
    + String(minutes).padStart(2, "0") + ":"
    + String(seconds).padStart(2, "0") + "."
    + String(centiseconds).padStart(2, "0");
}

function browserToken(request) {
  const cookie = request.headers.get("Cookie") || "";
  const match = cookie.match(/(?:^|;\s*)trsr_session=([^;]+)/);
  return match ? decodeURIComponent(match[1]) : null;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers });
    if (request.method === "GET" && url.pathname === "/health") return json({ status: "ok", service: "TRSR matchmaking server" });

    if (request.method === "GET" && url.pathname === "/public/overview") {
      const cutoff = new Date(Date.now() - 90000).toISOString();
      const counts = await env.DB.batch([
        env.DB.prepare("SELECT COUNT(*) AS total FROM queue_entries"),
        env.DB.prepare("SELECT COUNT(DISTINCT player_id) AS total FROM match_presence WHERE updated_at_utc >= ? AND state IN ('ready', 'arming', 'armed', 'running', 'restarted')").bind(cutoff)
      ]);
      const queue = Number(counts[0].results?.[0]?.total ?? 0);
      const racing = Number(counts[1].results?.[0]?.total ?? 0);
      return json({ online: queue + racing, queue, racing });
    }

    if (request.method === "GET" && url.pathname === "/public/players") {
      const cutoff = new Date(Date.now() - 90000).toISOString();
      const [queued, active] = await env.DB.batch([
        env.DB.prepare("SELECT q.speedrun_user_id, p.display_name FROM queue_entries q LEFT JOIN profiles p ON p.speedrun_user_id = q.speedrun_user_id ORDER BY q.joined_at DESC LIMIT 20"),
        env.DB.prepare("SELECT mp.player_id, mp.state, m.match_json FROM match_presence mp JOIN matches m ON m.player_id = mp.player_id WHERE mp.updated_at_utc >= ? AND mp.state IN ('ready', 'arming', 'armed', 'running', 'restarted') ORDER BY mp.updated_at_utc DESC LIMIT 20").bind(cutoff)
      ]);
      const players = new Map();
      for (const row of queued.results ?? []) {
        players.set(row.speedrun_user_id, {
          id: row.speedrun_user_id,
          name: row.display_name || row.speedrun_user_id,
          state: "in queue",
          game: "Tomb Raider: Anniversary"
        });
      }
      const activePlayers = [];
      for (const row of active.results ?? []) {
        let match;
        try { match = JSON.parse(row.match_json); } catch { continue; }
        const participant = match.players?.find((player) => player.playerId === row.player_id);
        if (participant?.speedrunUserId) activePlayers.push({ id: participant.speedrunUserId, state: row.state });
      }
      const ids = [...new Set(activePlayers.map((player) => player.id))];
      let names = new Map();
      if (ids.length) {
        const placeholders = ids.map(() => "?").join(",");
        const profiles = await env.DB.prepare("SELECT speedrun_user_id, display_name FROM profiles WHERE speedrun_user_id IN (" + placeholders + ")").bind(...ids).all();
        names = new Map((profiles.results ?? []).map((row) => [row.speedrun_user_id, row.display_name]));
      }
      for (const player of activePlayers) {
        players.set(player.id, {
          id: player.id,
          name: names.get(player.id) || player.id,
          state: player.state,
          game: "Tomb Raider: Anniversary"
        });
      }
      return json({ players: [...players.values()].slice(0, 20) });
    }

    if (request.method === "GET" && url.pathname === "/public/leaderboards") {
      const game = url.searchParams.get("game") || "anniversary";
      const level = url.searchParams.get("level") || "all";
      const category = url.searchParams.get("category") || "all";
      const subcategory = url.searchParams.get("subcategory") || "all";
      if (game !== "anniversary"
          || (level !== "all" && !LEVELS.includes(level))
          || (category !== "all" && !CATEGORIES.includes(category))
          || (subcategory !== "all" && !SUBCATEGORIES.includes(subcategory))) {
        return json({ error: "Invalid leaderboard filter." }, 400);
      }
      const results = await env.DB.prepare(
        "WITH best AS (" +
        " SELECT r.speedrun_user_id, p.display_name, r.level, r.category, r.subcategory," +
        " MIN(r.igt_milliseconds) AS best_igt_milliseconds, COUNT(*) AS wins" +
        " FROM match_results r" +
        " JOIN match_outcomes o ON o.match_id = r.match_id AND o.winner_player_id = r.player_id" +
        " LEFT JOIN profiles p ON p.speedrun_user_id = r.speedrun_user_id" +
        " WHERE (? = 'all' OR r.level = ?) AND (? = 'all' OR r.category = ?)" +
        " AND (? = 'all' OR r.subcategory = ?)" +
        " GROUP BY r.speedrun_user_id, r.level, r.category, r.subcategory" +
        "), ranked AS (SELECT *, ROW_NUMBER() OVER (" +
        " PARTITION BY level, category, subcategory ORDER BY best_igt_milliseconds ASC" +
        ") AS rank FROM best)" +
        " SELECT * FROM ranked WHERE rank <= ? ORDER BY level, category, subcategory, rank LIMIT 1200"
      ).bind(level, level, category, category, subcategory, subcategory, level === "all" ? 10 : 100).all();
      return json({ entries: (results.results ?? []).map((row) => ({
        rank: Number(row.rank),
        playerId: row.speedrun_user_id,
        playerName: row.display_name || row.speedrun_user_id,
        level: row.level,
        category: row.category,
        subcategory: row.subcategory,
        time: formatIgt(Number(row.best_igt_milliseconds)),
        wins: Number(row.wins)
      })) });
    }

    if (request.method === "GET" && url.pathname === "/auth/me") {
      const session = await sessionFor(env, browserToken(request));
      if (!session) return json({ error: "Not signed in." }, 401);
      const stats = await env.DB.prepare(
        "SELECT COUNT(*) AS finishes," +
        " SUM(CASE WHEN o.winner_player_id = r.player_id THEN 1 ELSE 0 END) AS winning_finishes" +
        " FROM match_results r LEFT JOIN match_outcomes o ON o.match_id = r.match_id" +
        " WHERE r.speedrun_user_id = ?"
      ).bind(session.speedrun_user_id).first();
      return json({
        profile: { id: session.speedrun_user_id, displayName: session.display_name },
        stats: {
          finishes: Number(stats?.finishes ?? 0),
          winningFinishes: Number(stats?.winning_finishes ?? 0)
        }
      });
    }

    if (request.method === "POST" && url.pathname === "/auth/logout") {
      if (request.headers.get("Origin") !== "https://trsr.app") {
        return json({ error: "Invalid origin." }, 403);
      }
      const token = browserToken(request);
      if (token) {
        await env.DB.prepare("DELETE FROM sessions WHERE token_hash = ?").bind(await hash(token)).run();
      }
      return new Response(JSON.stringify({ status: "signed_out" }), {
        headers: {
          ...headers,
          "Set-Cookie": "trsr_session=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax"
        }
      });
    }

    if (request.method === "POST" && url.pathname === "/auth/speedrun") {
      const browserOrigin = request.headers.get("Origin");
      if (browserOrigin && browserOrigin !== "https://trsr.app") {
        return json({ error: "Invalid origin." }, 403);
      }
      const data = await body(request);
      const apiKey = typeof data?.apiKey === "string" ? data.apiKey.trim() : "";
      if (!apiKey) return json({ error: "A Speedrun.com API key is required." }, 400);
      let response;
      try { response = await fetch("https://www.speedrun.com/api/v1/profile", { headers: { "X-API-Key": apiKey } }); }
      catch { return json({ error: "Could not reach Speedrun.com." }, 502); }
      if (!response.ok) return json({ error: "Speedrun.com rejected that API key." }, 401);
      const user = (await response.json()).data;
      if (!user?.id) return json({ error: "Speedrun.com returned an unexpected profile." }, 502);
      const profileCard = speedrunProfileCard(user, user.id);
      const displayName = profileCard.displayName;
      const token = crypto.randomUUID();
      const now = new Date().toISOString();
      const expiresAt = new Date(Date.now() + 90 * 86400000).toISOString();
      await env.DB.batch([
        env.DB.prepare(`INSERT INTO profiles
          (speedrun_user_id, display_name, avatar_url, country_code, linked_at)
          VALUES (?, ?, ?, ?, ?)
          ON CONFLICT(speedrun_user_id) DO UPDATE SET
            display_name = excluded.display_name,
            avatar_url = excluded.avatar_url,
            country_code = excluded.country_code,
            linked_at = excluded.linked_at`)
          .bind(user.id, displayName, profileCard.avatarUrl || null,
                profileCard.countryCode || null, now),
        env.DB.prepare(`DELETE FROM sessions WHERE expires_at <= ?`).bind(now),
        env.DB.prepare(`INSERT INTO sessions (token_hash, speedrun_user_id, created_at, expires_at) VALUES (?, ?, ?, ?)`).bind(await hash(token), user.id, now, expiresAt)
      ]);
      const profile = { id: user.id, displayName };
      if (browserOrigin === "https://trsr.app") {
        return new Response(JSON.stringify({ status: "linked", profile }), {
          headers: {
            ...headers,
            "Set-Cookie": "trsr_session=" + encodeURIComponent(token)
              + "; Max-Age=7776000; Path=/; HttpOnly; Secure; SameSite=Lax"
          }
        });
      }
      return json({ status: "linked", sessionToken: token, profile });
    }

    if (request.method === "POST" && url.pathname === "/auth/session") {
      const session = await sessionFor(env, (await body(request))?.sessionToken);
      return session ? json({ status: "valid", profile: { id: session.speedrun_user_id, displayName: session.display_name } }) : json({ error: "Your TRSR login has expired." }, 401);
    }

    if (request.method === "POST" && url.pathname === "/admin/overview") {
      const session = await sessionFor(env, (await body(request))?.sessionToken);
      if (!session) return json({ error: "Your TRSR login has expired." }, 401);
      if (session.display_name.toLocaleLowerCase() !== ADMIN_DISPLAY_NAME) {
        return json({ error: "Administrator access is required." }, 403);
      }
      const activeCutoff = new Date(Date.now() - 90000).toISOString();
      const [queued, activeMatches, running, recentResults] = await env.DB.batch([
        env.DB.prepare(`SELECT COUNT(*) AS total FROM queue_entries`),
        env.DB.prepare(`SELECT COUNT(DISTINCT match_id) AS total FROM match_presence WHERE updated_at_utc >= ?`)
          .bind(activeCutoff),
        env.DB.prepare(`SELECT COUNT(*) AS total FROM match_presence WHERE updated_at_utc >= ? AND state IN ('running', 'restarted')`)
          .bind(activeCutoff),
        env.DB.prepare(`SELECT COUNT(*) AS total FROM match_outcomes WHERE decided_at_utc >= ?`)
          .bind(new Date(Date.now() - 86400000).toISOString())
      ]);
      return json({
        status: "ok",
        overview: {
          queuedPlayers: Number(queued.results?.[0]?.total ?? 0),
          activeMatches: Number(activeMatches.results?.[0]?.total ?? 0),
          runnersInLevel: Number(running.results?.[0]?.total ?? 0),
          resultsLast24Hours: Number(recentResults.results?.[0]?.total ?? 0)
        }
      });
    }

    if (request.method === "POST" && url.pathname === "/queue/cancel") {
      const data = await body(request);
      const playerId = typeof data?.playerId === "string" ? data.playerId.trim() : "";
      const session = await sessionFor(env, data?.sessionToken);
      if (!playerId) return json({ error: "A player ID is required." }, 400);
      if (!session) return json({ error: "Link your Speedrun.com account before matchmaking." }, 401);
      await env.DB.prepare(`DELETE FROM queue_entries WHERE player_id = ? AND speedrun_user_id = ?`)
        .bind(playerId, session.speedrun_user_id).run();
      return json({ status: "cancelled" });
    }

    if (request.method === "POST" && url.pathname === "/queue") {
      const data = await body(request);
      const playerId = typeof data?.playerId === "string" ? data.playerId.trim() : "";
      const { queueType, level, category, subcategory } = data ?? {};
      if (!playerId) return json({ error: "A player ID is required." }, 400);
      const session = await sessionFor(env, data?.sessionToken);
      if (!session) return json({ error: "Link your Speedrun.com account before matchmaking." }, 401);
      if (queueType !== "random" && queueType !== "ruleset") return json({ error: "Invalid queue type." }, 400);
      if (queueType === "ruleset" && (!CATEGORIES.includes(category) || !SUBCATEGORIES.includes(subcategory))) return json({ error: "Choose a valid category and subcategory." }, 400);
      const key = queueKey(queueType, category, subcategory);
      await env.DB.prepare(`DELETE FROM queue_entries WHERE player_id = ?`).bind(playerId).run();
      const waiting = await env.DB.prepare(`SELECT * FROM queue_entries WHERE queue_key = ?`).bind(key).first();
      if (!waiting) {
        await env.DB.prepare(`INSERT INTO queue_entries (queue_key, player_id, speedrun_user_id, queue_type, category, subcategory, joined_at) VALUES (?, ?, ?, ?, ?, ?, ?)`)
          .bind(key, playerId, session.speedrun_user_id, queueType, category ?? null, subcategory ?? null, new Date().toISOString()).run();
        return json({ status: "waiting", message: "Waiting for an opponent." });
      }
      if (waiting.player_id === playerId) return json({ status: "waiting", message: "You are already waiting in this queue." });
      const createdAtUtc = new Date().toISOString();
      const match = { id: crypto.randomUUID(), phase: "proposal", proposalExpiresAtUtc: new Date(Date.now() + MATCH_ACCEPT_TIMEOUT_MS).toISOString(), acceptedPlayerIds: [], players: [{ playerId: waiting.player_id, speedrunUserId: waiting.speedrun_user_id }, { playerId, speedrunUserId: session.speedrun_user_id }], level: randomItem(LEVELS), category: queueType === "random" ? randomItem(CATEGORIES) : category, subcategory: queueType === "random" ? randomItem(SUBCATEGORIES) : subcategory, difficulty: "Easy", createdAtUtc };
      const matchJson = JSON.stringify(match);
      await env.DB.batch([
        env.DB.prepare(`DELETE FROM queue_entries WHERE queue_key = ?`).bind(key),
        env.DB.prepare(`INSERT OR REPLACE INTO matches (player_id, match_json, created_at) VALUES (?, ?, ?)`).bind(waiting.player_id, matchJson, createdAtUtc),
        env.DB.prepare(`INSERT OR REPLACE INTO matches (player_id, match_json, created_at) VALUES (?, ?, ?)`).bind(playerId, matchJson, createdAtUtc)
      ]);
      return json({ status: "proposal", proposalExpiresAtUtc: match.proposalExpiresAtUtc });
    }

    if (request.method === "POST"
        && url.pathname.startsWith("/matches/")
        && url.pathname.endsWith("/proposal")) {
      const playerId = decodeURIComponent(
        url.pathname.slice("/matches/".length, -"/proposal".length)
      );
      const data = await body(request);
      const session = await sessionFor(env, data?.sessionToken);
      const decision = data?.decision;
      if (!session) return json({ error: "Link your Speedrun.com account before responding to a match." }, 401);
      if (decision !== "accept" && decision !== "decline") return json({ error: "Choose accept or decline." }, 400);
      const stored = await env.DB.prepare(`SELECT match_json FROM matches WHERE player_id = ?`)
        .bind(playerId).first();
      if (!stored) return json({ status: "none" });
      const match = JSON.parse(stored.match_json);
      const isParticipant = match.players?.some((player) =>
        player.playerId === playerId && player.speedrunUserId === session.speedrun_user_id
      );
      if (!isParticipant) return json({ error: "This offer does not belong to your account." }, 403);
      if (match.phase !== "proposal") {
        return json({ status: "matched", match: await matchForClient(env, match) });
      }
      if (Date.parse(match.proposalExpiresAtUtc) <= Date.now()) {
        await deleteMatchState(env, match);
        return json({ status: "expired" });
      }
      if (decision === "decline") {
        await deleteMatchState(env, match);
        return json({ status: "declined" });
      }

      const accepted = new Set(match.acceptedPlayerIds ?? []);
      accepted.add(playerId);
      match.acceptedPlayerIds = [...accepted];
      const bothAccepted = match.players.every((player) => accepted.has(player.playerId));
      if (bothAccepted) {
        match.phase = "matched";
        match.acceptedAtUtc = new Date().toISOString();
        delete match.proposalExpiresAtUtc;
        delete match.acceptedPlayerIds;
      }
      const matchJson = JSON.stringify(match);
      await env.DB.batch(match.players.map((player) =>
        env.DB.prepare(`UPDATE matches SET match_json = ? WHERE player_id = ?`)
          .bind(matchJson, player.playerId)
      ));
      return bothAccepted
        ? json({ status: "matched", match: await matchForClient(env, match) })
        : json({ status: "accepted", proposalExpiresAtUtc: match.proposalExpiresAtUtc });
    }

    if (request.method === "POST"
        && url.pathname.startsWith("/matches/")
        && url.pathname.endsWith("/presence")) {
      const playerId = decodeURIComponent(
        url.pathname.slice("/matches/".length, -"/presence".length)
      );
      const data = await body(request);
      const session = await sessionFor(env, data?.sessionToken);
      const state = typeof data?.state === "string" ? data.state : "";
      const igtMilliseconds = data?.igtMilliseconds == null
        ? null : Number(data.igtMilliseconds);
      const allowedStates = new Set(["waiting", "ready", "arming", "armed", "running", "restarted", "finished"]);

      if (!playerId) return json({ error: "A player is required." }, 400);
      if (!session) return json({ error: "Link your Speedrun.com account before updating match status." }, 401);
      if (!allowedStates.has(state)) return json({ error: "Invalid match status." }, 400);
      if (igtMilliseconds != null
          && (!Number.isInteger(igtMilliseconds) || igtMilliseconds < 0 || igtMilliseconds > 86400000)) {
        return json({ error: "Invalid in-game time." }, 400);
      }

      const stored = await env.DB.prepare(`SELECT match_json FROM matches WHERE player_id = ?`)
        .bind(playerId).first();
      if (!stored) return json({ status: "none" });

      const match = JSON.parse(stored.match_json);
      const isParticipant = match.players?.some((player) =>
        player.playerId === playerId && player.speedrunUserId === session.speedrun_user_id
      );
      if (!isParticipant) return json({ error: "This status does not belong to your active match." }, 403);
      if (match.phase === "proposal") {
        return json({ error: "Both players must accept the match first." }, 409);
      }

      const nowMilliseconds = Date.now();
      const now = new Date(nowMilliseconds).toISOString();
      let navigationSchedule = await env.DB.prepare(
        `SELECT start_at_utc FROM match_navigation_schedule WHERE match_id = ?`
      ).bind(match.id).first();

      if (state === "running") {
        if (!navigationSchedule) {
          return json({
            status: "start_rejected",
            error: "Both players must pass the Ready check before starting."
          }, 409);
        }
        if (nowMilliseconds + EARLY_START_TOLERANCE_MS
            < Date.parse(navigationSchedule.start_at_utc)) {
          return json({
            status: "start_rejected",
            error: "The level was entered before the shared GO signal."
          }, 409);
        }
        await env.DB.prepare(
          `INSERT OR IGNORE INTO match_race_starts (match_id, player_id, started_at_utc)
           VALUES (?, ?, ?)`
        ).bind(match.id, playerId, now).run();
      }

      await env.DB.prepare(
        `INSERT INTO match_presence (match_id, player_id, state, igt_milliseconds, updated_at_utc)
         VALUES (?, ?, ?, ?, ?)
         ON CONFLICT(match_id, player_id) DO UPDATE SET
           state = excluded.state,
           igt_milliseconds = excluded.igt_milliseconds,
           updated_at_utc = excluded.updated_at_utc`
      ).bind(match.id, playerId, state, igtMilliseconds, now).run();

      const rows = await env.DB.prepare(
        `SELECT player_id, state, igt_milliseconds, updated_at_utc
         FROM match_presence WHERE match_id = ?`
      ).bind(match.id).all();
      const presenceByPlayer = new Map(
        (rows.results ?? []).map((row) => [row.player_id, row])
      );
      const bothPlayersReady = match.players.length === 2 && match.players.every(
        (player) => presenceByPlayer.get(player.playerId)?.state === "ready"
      );
      if (bothPlayersReady && !navigationSchedule) {
        await env.DB.prepare(
          `INSERT OR IGNORE INTO match_navigation_schedule (match_id, start_at_utc, created_at_utc)
           VALUES (?, ?, ?)`
        ).bind(
          match.id,
          new Date(nowMilliseconds + GO_COUNTDOWN_MS).toISOString(),
          now
        ).run();
        navigationSchedule = await env.DB.prepare(
          `SELECT start_at_utc FROM match_navigation_schedule WHERE match_id = ?`
        ).bind(match.id).first();
      }
      const navigationStartAtUtc = navigationSchedule?.start_at_utc ?? null;
      const navigationDelayMilliseconds = navigationStartAtUtc
        ? Math.max(0, Date.parse(navigationStartAtUtc) - nowMilliseconds)
        : null;

      const starts = await env.DB.prepare(
        `SELECT player_id, started_at_utc FROM match_race_starts WHERE match_id = ?`
      ).bind(match.id).all();
      const startsByPlayer = new Map(
        (starts.results ?? []).map((row) => [row.player_id, row.started_at_utc])
      );
      const raceState = await resolveExpiredFinishWindow(env, match.id, nowMilliseconds);
      const finishWindow = raceState.window;
      return json({
        status: "matched",
        navigationStartAtUtc,
        navigationDelayMilliseconds,
        winnerPlayerId: raceState.outcome?.winner_player_id ?? null,
        winnerIgtMilliseconds: raceState.outcome?.winner_igt_milliseconds ?? null,
        decidedAtUtc: raceState.outcome?.decided_at_utc ?? null,
        firstFinisherPlayerId: finishWindow?.first_finisher_player_id ?? null,
        finishDeadlineUtc: finishWindow?.finish_deadline_utc ?? null,
        players: match.players.map((player) => {
          const presence = presenceByPlayer.get(player.playerId);
          return {
            playerId: player.playerId,
            state: presence?.state ?? "waiting",
            igtMilliseconds: presence?.igt_milliseconds ?? null,
            updatedAtUtc: presence?.updated_at_utc ?? null,
            startedAtUtc: startsByPlayer.get(player.playerId) ?? null
          };
        })
      });
    }
    if (request.method === "POST"
        && url.pathname.startsWith("/matches/")
        && url.pathname.endsWith("/cancel")) {
      const playerId = decodeURIComponent(
        url.pathname.slice("/matches/".length, -"/cancel".length)
      );
      const data = await body(request);
      const session = await sessionFor(env, data?.sessionToken);
      if (!playerId) return json({ error: "A player is required." }, 400);
      if (!session) return json({ error: "Link your Speedrun.com account before leaving staging." }, 401);

      const stored = await env.DB.prepare(`SELECT match_json FROM matches WHERE player_id = ?`)
        .bind(playerId).first();
      if (!stored) return json({ status: "none" });
      const match = JSON.parse(stored.match_json);
      const isParticipant = match.players?.some((player) =>
        player.playerId === playerId && player.speedrunUserId === session.speedrun_user_id
      );
      if (!isParticipant) return json({ error: "This cancellation does not belong to your active match." }, 403);

      const [outcome, raceStarts] = await env.DB.batch([
        env.DB.prepare(`SELECT 1 AS present FROM match_outcomes WHERE match_id = ?`).bind(match.id),
        env.DB.prepare(`SELECT COUNT(*) AS total FROM match_race_starts WHERE match_id = ?`).bind(match.id)
      ]);
      if (outcome.results?.length || Number(raceStarts.results?.[0]?.total ?? 0) > 0) {
        return json({ error: "The race has started. Use Forfeit match instead." }, 409);
      }

      await env.DB.batch([
        env.DB.prepare(`DELETE FROM match_presence WHERE match_id = ?`).bind(match.id),
        env.DB.prepare(`DELETE FROM match_navigation_schedule WHERE match_id = ?`).bind(match.id),
        env.DB.prepare(`DELETE FROM match_start_schedule WHERE match_id = ?`).bind(match.id),
        env.DB.prepare(`DELETE FROM match_race_starts WHERE match_id = ?`).bind(match.id),
        env.DB.prepare(`DELETE FROM match_finish_windows WHERE match_id = ?`).bind(match.id),
        ...match.players.map((player) => env.DB.prepare(`DELETE FROM matches WHERE player_id = ?`).bind(player.playerId))
      ]);
      return json({ status: "cancelled" });
    }
    if (request.method === "POST"
        && url.pathname.startsWith("/matches/")
        && url.pathname.endsWith("/forfeit")) {
      const playerId = decodeURIComponent(
        url.pathname.slice("/matches/".length, -"/forfeit".length)
      );
      const data = await body(request);
      const session = await sessionFor(env, data?.sessionToken);
      if (!playerId) return json({ error: "A player is required." }, 400);
      if (!session) return json({ error: "Link your Speedrun.com account before forfeiting." }, 401);

      const stored = await env.DB.prepare(`SELECT match_json FROM matches WHERE player_id = ?`)
        .bind(playerId).first();
      if (!stored) return json({ error: "No active match was found." }, 404);
      const match = JSON.parse(stored.match_json);
      const isParticipant = match.players?.some((player) =>
        player.playerId === playerId && player.speedrunUserId === session.speedrun_user_id
      );
      if (!isParticipant) return json({ error: "This forfeit does not belong to your active match." }, 403);
      const opponent = match.players?.find((player) => player.playerId !== playerId);
      if (!opponent) return json({ error: "A two-player match is required to forfeit." }, 409);

      const existingOutcome = await env.DB.prepare(
        `SELECT winner_player_id, decided_at_utc FROM match_outcomes WHERE match_id = ?`
      ).bind(match.id).first();
      if (existingOutcome) {
        return json({
          status: "already_decided",
          winnerPlayerId: existingOutcome.winner_player_id,
          decidedAtUtc: existingOutcome.decided_at_utc
        });
      }

      const now = new Date().toISOString();
      await env.DB.batch([
        env.DB.prepare(
          `INSERT OR IGNORE INTO match_forfeits (match_id, player_id, forfeited_at_utc)
           VALUES (?, ?, ?)`
        ).bind(match.id, playerId, now),
        env.DB.prepare(
          `INSERT OR IGNORE INTO match_outcomes
            (match_id, winner_player_id, winner_igt_milliseconds, decided_at_utc)
           VALUES (?, ?, ?, ?)`
        ).bind(match.id, opponent.playerId, 0, now)
      ]);
      const outcome = await env.DB.prepare(
        `SELECT winner_player_id, winner_igt_milliseconds, decided_at_utc
         FROM match_outcomes WHERE match_id = ?`
      ).bind(match.id).first();
      return json({
        status: outcome.winner_player_id === opponent.playerId ? "forfeited" : "already_decided",
        winnerPlayerId: outcome.winner_player_id,
        decidedAtUtc: outcome.decided_at_utc
      });
    }
    if (request.method === "POST"
        && url.pathname.startsWith("/matches/")
        && url.pathname.endsWith("/result")) {
      const playerId = decodeURIComponent(
        url.pathname.slice("/matches/".length, -"/result".length)
      );
      const data = await body(request);
      const session = await sessionFor(env, data?.sessionToken);
      const igtMilliseconds = Number(data?.igtMilliseconds);
      const buildHash = typeof data?.buildHash === "string" ? data.buildHash : "";

      if (!playerId) return json({ error: "A player is required." }, 400);
      if (!session) return json({ error: "Link your Speedrun.com account before submitting a result." }, 401);
      if (!Number.isInteger(igtMilliseconds) || igtMilliseconds <= 0 || igtMilliseconds > 86400000) {
        return json({ error: "The submitted in-game time is invalid." }, 400);
      }

      const stored = await env.DB.prepare(`SELECT match_json FROM matches WHERE player_id = ?`)
        .bind(playerId).first();
      if (!stored) return json({ error: "No active match was found." }, 404);

      const match = JSON.parse(stored.match_json);
      const isParticipant = match.players?.some((player) =>
        player.playerId === playerId && player.speedrunUserId === session.speedrun_user_id
      );
      if (!isParticipant) {
        return json({ error: "This result does not belong to your active match." }, 403);
      }
      if (data?.level !== match.level
          || data?.category !== match.category
          || data?.subcategory !== match.subcategory) {
        return json({ error: "This result does not match the active ruleset." }, 409);
      }

      const nowMilliseconds = Date.now();
      const resolvedRace = await resolveExpiredFinishWindow(
        env, match.id, nowMilliseconds
      );
      if (resolvedRace.outcome) {
        return json({
          status: "already_decided",
          winnerPlayerId: resolvedRace.outcome.winner_player_id,
          winnerIgtMilliseconds: resolvedRace.outcome.winner_igt_milliseconds,
          decidedAtUtc: resolvedRace.outcome.decided_at_utc,
          youWon: resolvedRace.outcome.winner_player_id === playerId
        });
      }

      const ownStart = await env.DB.prepare(
        `SELECT started_at_utc FROM match_race_starts
         WHERE match_id = ? AND player_id = ?`
      ).bind(match.id, playerId).first();
      if (!ownStart) {
        return json({ error: "Ready up and wait for GO before submitting a ranked result." }, 409);
      }

      const now = new Date(nowMilliseconds).toISOString();
      const write = await env.DB.prepare(
        `INSERT OR IGNORE INTO match_results
          (match_id, speedrun_user_id, player_id, level, category, subcategory,
           igt_milliseconds, build_hash, confirmed_at_utc)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
      ).bind(
        match.id, session.speedrun_user_id, playerId, match.level, match.category,
        match.subcategory, igtMilliseconds, buildHash, now
      ).run();

      let finishWindow = await env.DB.prepare(
        `SELECT first_finisher_player_id, first_finish_igt_milliseconds,
                first_finished_at_utc, finish_deadline_utc
         FROM match_finish_windows WHERE match_id = ?`
      ).bind(match.id).first();

      if (!finishWindow) {
        const starts = await env.DB.prepare(
          `SELECT player_id, started_at_utc FROM match_race_starts WHERE match_id = ?`
        ).bind(match.id).all();
        const startsByPlayer = new Map(
          (starts.results ?? []).map((row) => [row.player_id, Date.parse(row.started_at_utc)])
        );
        const opponent = match.players.find((player) => player.playerId !== playerId);
        const startSkewMilliseconds = opponent && startsByPlayer.has(opponent.playerId)
          ? Math.max(0, startsByPlayer.get(opponent.playerId) - startsByPlayer.get(playerId))
          : 0;
        const finishWindowMilliseconds = Math.min(
          MAXIMUM_FINISH_WINDOW_MS,
          Math.max(MINIMUM_FINISH_WINDOW_MS, FINISH_GRACE_MS + startSkewMilliseconds)
        );
        await env.DB.prepare(
          `INSERT OR IGNORE INTO match_finish_windows
            (match_id, first_finisher_player_id, first_finish_igt_milliseconds,
             first_finished_at_utc, finish_deadline_utc)
           VALUES (?, ?, ?, ?, ?)`
        ).bind(
          match.id,
          playerId,
          igtMilliseconds,
          now,
          new Date(nowMilliseconds + finishWindowMilliseconds).toISOString()
        ).run();
        finishWindow = await env.DB.prepare(
          `SELECT first_finisher_player_id, first_finish_igt_milliseconds,
                  first_finished_at_utc, finish_deadline_utc
           FROM match_finish_windows WHERE match_id = ?`
        ).bind(match.id).first();
      }

      const submittedResults = await env.DB.prepare(
        `SELECT player_id, igt_milliseconds FROM match_results WHERE match_id = ?`
      ).bind(match.id).all();
      const results = submittedResults.results ?? [];
      if (results.length < 2) {
        return json({
          status: write.meta.changes > 0 ? "provisional" : "already_submitted",
          firstFinisherPlayerId: finishWindow.first_finisher_player_id,
          finishDeadlineUtc: finishWindow.finish_deadline_utc
        });
      }

      const firstResult = results.find(
        (result) => result.player_id === finishWindow.first_finisher_player_id
      );
      const secondResult = results.find(
        (result) => result.player_id !== finishWindow.first_finisher_player_id
      );
      const winner = Number(secondResult.igt_milliseconds) < Number(firstResult.igt_milliseconds)
        ? secondResult : firstResult;
      await env.DB.prepare(
        `INSERT OR IGNORE INTO match_outcomes
          (match_id, winner_player_id, winner_igt_milliseconds, decided_at_utc)
         VALUES (?, ?, ?, ?)`
      ).bind(match.id, winner.player_id, winner.igt_milliseconds, now).run();
      const outcome = await env.DB.prepare(
        `SELECT winner_player_id, winner_igt_milliseconds, decided_at_utc
         FROM match_outcomes WHERE match_id = ?`
      ).bind(match.id).first();

      return json({
        status: "completed",
        winnerPlayerId: outcome.winner_player_id,
        winnerIgtMilliseconds: outcome.winner_igt_milliseconds,
        decidedAtUtc: outcome.decided_at_utc,
        youWon: outcome.winner_player_id === playerId
      });
    }
    if (request.method === "GET" && url.pathname.startsWith("/matches/")) {
      const playerId = decodeURIComponent(url.pathname.slice("/matches/".length));
      const result = await env.DB.prepare(`SELECT match_json FROM matches WHERE player_id = ?`).bind(playerId).first();
      if (!result) return json({ status: "none" });
      const match = JSON.parse(result.match_json);
      if (match.phase === "proposal") {
        if (Date.parse(match.proposalExpiresAtUtc) <= Date.now()) {
          await deleteMatchState(env, match);
          return json({ status: "expired" });
        }
        return json({
          status: "proposal",
          proposalExpiresAtUtc: match.proposalExpiresAtUtc,
          acceptedPlayerIds: match.acceptedPlayerIds ?? []
        });
      }
      const raceState = await resolveExpiredFinishWindow(env, match.id);
      const outcome = raceState.outcome;
      const clientMatch = await matchForClient(env, match);
      return outcome
        ? json({ status: "completed", match: clientMatch, winnerPlayerId: outcome.winner_player_id, decidedAtUtc: outcome.decided_at_utc })
        : json({ status: "matched", match: clientMatch });
    }
    return json({ error: "Not found." }, 404);
  }
};
