#!/usr/bin/env python3
"""
本地调用 POST /api/v1/auth/register（需服务端 config 中 registerApiKey 非空）。

用法示例（在 TomatoServer 仓库根目录执行）:
  python scripts/register_user.py --username alice --password 'Secret#1'
  python scripts/register_user.py --base-url https://127.0.0.1:8443 --register-key my-key u2 p2

默认从 config/server.dev.json 读取 httpsPort、registerApiKey；Base URL 默认 https://127.0.0.1:<port>
"""
from __future__ import annotations

import argparse
import json
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_config(path: Path) -> dict:
    if not path.is_file():
        print(f"Config not found: {path}", file=sys.stderr)
        sys.exit(2)
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def default_base_url(cfg: dict) -> str:
    port = int(cfg.get("httpsPort", 8443))
    return f"https://127.0.0.1:{port}"


def post_json(url: str, headers: dict, body: dict) -> tuple[int, str]:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={**headers, "Content-Type": "application/json"},
        method="POST",
    )
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace") if e.fp else ""
        return e.code, raw


def main() -> None:
    p = argparse.ArgumentParser(description="Register a user via local TomatoServer register API.")
    p.add_argument("--config", type=Path, default=repo_root() / "config" / "server.dev.json")
    p.add_argument("--base-url", help="Override, e.g. https://192.168.31.124:8443")
    p.add_argument("--register-key", help="Override X-Tomato-Register-Key (default from config registerApiKey)")
    p.add_argument("username", nargs="?", help="New username")
    p.add_argument("password", nargs="?", help="Plain password (only sent to server over HTTPS)")
    p.add_argument("-u", "--username-flag", dest="username_opt", help="Username (alternative to positional)")
    p.add_argument("-p", "--password-flag", dest="password_opt", help="Password (alternative to positional)")
    args = p.parse_args()

    cfg = load_config(args.config)
    key = (args.register_key or cfg.get("registerApiKey") or "").strip()
    if not key:
        print(
            "registerApiKey is empty in config — server returns 403 for registration. "
            "Set registerApiKey in config or pass --register-key.",
            file=sys.stderr,
        )
        sys.exit(2)

    base = (args.base_url or default_base_url(cfg)).rstrip("/")
    username = (args.username_opt or args.username or "").strip()
    password = (args.password_opt or args.password or "")
    if not username or not password:
        p.print_help()
        print("\nusername and password are required.", file=sys.stderr)
        sys.exit(2)

    url = f"{base}/api/v1/auth/register"
    code, text = post_json(
        url,
        {"X-Tomato-Register-Key": key},
        {"username": username, "password": password},
    )
    print(text)
    if code != 200:
        sys.exit(1)


if __name__ == "__main__":
    main()
