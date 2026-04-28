"""Print users hesphoros/ruansiqi and related friendship rows. Run from repo root."""
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CANDIDATES = [
    ROOT / "storage" / "db" / "tomato.db",
    ROOT / "build" / "Debug" / "storage" / "db" / "tomato.db",
    ROOT / "build" / "Release" / "storage" / "db" / "tomato.db",
]


def main() -> None:
    db = next((p for p in CANDIDATES if p.is_file()), None)
    if db is None:
        print("No database file found. Tried:")
        for p in CANDIDATES:
            print(f"  {p}")
        sys.exit(2)

    print(f"Database: {db}\n")
    con = sqlite3.connect(str(db))
    con.row_factory = sqlite3.Row
    cur = con.cursor()

    cols = {r[1] for r in cur.execute("PRAGMA table_info(users)")}
    user_select = "id, username, substr(password_hash, 1, 24) || '...' AS hash_prefix"
    if "bio" in cols:
        user_select += ", bio"
    if "avatar_media_url" in cols:
        user_select += ", avatar_media_url"

    print("--- users (hesphoros / ruansiqi) ---")
    rows = cur.execute(
        f"""
        SELECT {user_select}
        FROM users
        WHERE username IN ('hesphoros', 'ruansiqi')
        ORDER BY id
        """
    ).fetchall()
    if not rows:
        print("(no rows)")
    for r in rows:
        print(dict(r))

    print("\n--- friendships involving hesphoros or ruansiqi ---")
    rows = cur.execute(
        """
        SELECT f.id, f.user_id, u1.username AS user_name,
               f.friend_id, u2.username AS friend_name, f.status
        FROM friendships f
        JOIN users u1 ON u1.id = f.user_id
        JOIN users u2 ON u2.id = f.friend_id
        WHERE u1.username IN ('hesphoros', 'ruansiqi')
           OR u2.username IN ('hesphoros', 'ruansiqi')
        ORDER BY f.id
        """
    ).fetchall()
    if not rows:
        print("(no rows)")
    for r in rows:
        print(dict(r))

    total = cur.execute("SELECT COUNT(*) FROM friendships").fetchone()[0]
    print(f"\n--- total friendships rows: {total} ---")
    con.close()


if __name__ == "__main__":
    main()
