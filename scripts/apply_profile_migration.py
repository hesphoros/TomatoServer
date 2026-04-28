#!/usr/bin/env python3
"""
Ensure `users` has `bio` and `avatar_media_url` (migration 003), and record it in schema_migrations.

Use when the server returns 503 about missing profile columns — e.g. DB was created before 003 existed,
or migrations could not run from your working directory.

  python scripts/apply_profile_migration.py
  python scripts/apply_profile_migration.py --db D:\\path\\to\\tomato.db
"""
from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from pathlib import Path

MIGRATION_NAME = "003_user_profile.sql"
REPO_ROOT = Path(__file__).resolve().parents[1]


def load_sqlite_path(config_path: Path) -> Path:
    with config_path.open(encoding="utf-8") as f:
        cfg = json.load(f)
    rel = (cfg.get("sqlitePath") or "storage/db/tomato.db").strip()
    p = Path(rel)
    return p if p.is_absolute() else (REPO_ROOT / p)


def table_columns(cur: sqlite3.Cursor, table: str) -> set[str]:
    return {r[1] for r in cur.execute(f"PRAGMA table_info({table})")}


def main() -> None:
    p = argparse.ArgumentParser(description="Apply users profile columns (migration 003) to tomato.db")
    p.add_argument("--config", type=Path, default=REPO_ROOT / "config" / "server.dev.json")
    p.add_argument("--db", type=Path, default=None)
    args = p.parse_args()

    db_path = args.db if args.db is not None else load_sqlite_path(args.config)
    if not db_path.is_file():
        print(f"Database not found: {db_path}", file=sys.stderr)
        sys.exit(2)

    con = sqlite3.connect(str(db_path))
    cur = con.cursor()
    cols = table_columns(cur, "users")
    changed = False

    if "bio" not in cols:
        print("Adding column: users.bio")
        cur.execute("ALTER TABLE users ADD COLUMN bio TEXT NOT NULL DEFAULT ''")
        changed = True
    else:
        print("Column already present: users.bio")

    if "avatar_media_url" not in cols:
        print("Adding column: users.avatar_media_url")
        cur.execute("ALTER TABLE users ADD COLUMN avatar_media_url TEXT")
        changed = True
    else:
        print("Column already present: users.avatar_media_url")

    # Record migration so Migrator will skip re-running the same file on next server start
    cur.execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "name TEXT PRIMARY KEY,"
        "applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ")"
    )
    cur.execute(
        "INSERT OR IGNORE INTO schema_migrations(name) VALUES (?)",
        (MIGRATION_NAME,),
    )
    con.commit()
    con.close()

    print(f"\nDone. Database: {db_path}")
    if changed:
        print("Restart tomato_server.exe and try updating profile / avatar again.")
    else:
        print("No ALTER needed. If the server still returns 503, restart it to clear any in-memory cache.")


if __name__ == "__main__":
    main()
