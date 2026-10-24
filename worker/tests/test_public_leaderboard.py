"""Exercise the public leaderboard query against SQLite's real SQL engine."""

import json
from pathlib import Path
import re
import sqlite3
import unittest


WORKER_DIR = Path(__file__).resolve().parents[1]


def leaderboard_sql():
    source = (WORKER_DIR / "worker.js").read_text(encoding="utf-8")
    route = source.index('url.pathname === "/public/leaderboards"')
    start = source.index("const results = await env.DB.prepare(", route)
    end = source.index(").bind(", start)
    quoted_parts = re.findall(r'"(?:[^"\\]|\\.)*"', source[start:end])
    return "".join(json.loads(part) for part in quoted_parts)


class PublicLeaderboardTest(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript((WORKER_DIR / "schema.sql").read_text(encoding="utf-8"))
        for name in ("0002_match_results.sql", "0005_match_outcomes.sql"):
            self.db.executescript((WORKER_DIR / "migrations" / name).read_text(encoding="utf-8"))

        self.db.executemany(
            "INSERT INTO profiles VALUES (?, ?, ?)",
            [("a", "Runner A", "2026-01-01"), ("b", "Runner B", "2026-01-01")],
        )
        results = [
            ("m1", "a", "pa", "Croft Manor", "Any%", "Bug Jump", 7000, "hash", "2026-01-01"),
            ("m2", "b", "pb", "Croft Manor", "Any%", "Bug Jump", 8000, "hash", "2026-01-01"),
            ("m3", "a", "pa", "Croft Manor", "100%", "Glitchless", 20000, "hash", "2026-01-01"),
        ]
        self.db.executemany("INSERT INTO match_results VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", results)
        self.db.executemany(
            "INSERT INTO match_outcomes VALUES (?, ?, ?, ?)",
            [("m1", "pa", 7000, "2026-01-01"), ("m2", "pb", 8000, "2026-01-01"),
             ("m3", "pa", 20000, "2026-01-01"), ("forfeit", "pb", 0, "2026-01-01")],
        )

    def tearDown(self):
        self.db.close()

    def rows(self, level="all", category="all", subcategory="all", per_ruleset=10):
        return self.db.execute(
            leaderboard_sql(),
            (level, level, category, category, subcategory, subcategory, per_ruleset),
        ).fetchall()

    def test_all_rulesets_and_croft_manor_filter(self):
        rows = self.rows()
        self.assertEqual(len(rows), 3)
        self.assertEqual([row["rank"] for row in rows], [1, 1, 2])
        self.assertEqual([row["speedrun_user_id"] for row in self.rows("Croft Manor", "100%", "Glitchless")], ["a"])

    def test_forfeit_without_finish_is_not_ranked(self):
        self.assertEqual([row["wins"] for row in self.rows("Croft Manor", "Any%", "Bug Jump")], [1, 1])


if __name__ == "__main__":
    unittest.main()
