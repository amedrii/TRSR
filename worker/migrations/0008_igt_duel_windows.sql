-- Server-owned staging and finish-window state for IGT duels.
CREATE TABLE IF NOT EXISTS match_race_starts (
    match_id TEXT NOT NULL,
    player_id TEXT NOT NULL,
    started_at_utc TEXT NOT NULL,
    PRIMARY KEY (match_id, player_id)
);

CREATE INDEX IF NOT EXISTS match_race_starts_by_match
    ON match_race_starts (match_id);

CREATE TABLE IF NOT EXISTS match_finish_windows (
    match_id TEXT PRIMARY KEY,
    first_finisher_player_id TEXT NOT NULL,
    first_finish_igt_milliseconds INTEGER NOT NULL,
    first_finished_at_utc TEXT NOT NULL,
    finish_deadline_utc TEXT NOT NULL
);
