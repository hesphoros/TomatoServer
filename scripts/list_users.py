#!/usr/bin/env python3
"""
列出 SQLite 中所有用户。

重要：password_hash 列为 Argon2id 等单向哈希，无法还原明文密码。
本脚本只打印 id / username / hash 前缀 / 创建时间（及 profile 列若存在）。

用法（仓库根目录）:
  python scripts/list_users.py
  python scripts/list_users.py --db D:/path/to/tomato.db
"""
from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_config(path: Path) -> dict:
    if not path.is_file():
        print(f"Config not found: {path}", file=sys.stderr)
        sys.exit(2)
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def resolve_db_path(cfg: dict, override: Path | None) -> Path:
    if override is not None:
        return override
    rel = (cfg.get("sqlitePath") or "storage/db/tomato.db").strip()
    p = Path(rel)
    if p.is_absolute():
        return p
    return repo_root() / p


def main() -> None:
    p = argparse.ArgumentParser(
        description="List TomatoServer users (passwords are NOT stored in plaintext — only hashes).",
    )
    p.add_argument("--config", type=Path, default=repo_root() / "config" / "server.dev.json")
    p.add_argument("--db", type=Path, default=None, help="Override path to tomato.db")
    args = p.parse_args()

    cfg = load_config(args.config)
    db_path = resolve_db_path(cfg, args.db)
    if not db_path.is_file():
        print(f"Database file not found: {db_path}", file=sys.stderr)
        sys.exit(2)

    con = sqlite3.connect(str(db_path))
    con.row_factory = sqlite3.Row
    cur = con.cursor()
    cols = {r[1] for r in cur.execute("PRAGMA table_info(users)")}
    select = ["id", "username", "password_hash", "created_at"]
    if "bio" in cols:
        select.append("bio")
    if "avatar_media_url" in cols:
        select.append("avatar_media_url")
    q = f"SELECT {', '.join(select)} FROM users ORDER BY id"

    print(f"Database: {db_path}")
    print("Note: password_hash is one-way (Argon2id). Plaintext passwords cannot be read from DB.\n")

    for row in cur.execute(q):
        d = dict(row)
        ph = d.get("password_hash") or ""
        d["password_hash"] = (ph[:48] + "...") if len(ph) > 48 else ph
        print(dict(d))
    con.close()


if __name__ == "__main__":
    main()
