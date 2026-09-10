#!/usr/bin/env python3
"""Interactive Feiniu API smoke test. Credentials stay in memory and are never written."""

from __future__ import annotations

import getpass
import hashlib
import json
import random
import sys
import time
import urllib.error
import urllib.request

DEFAULT_BASE = "http://192.168.31.137:5666"
API_KEY = "NDzZTVxnRKP8Z0jXg1VAMonaG8akvh"
API_SECRET = "16CCEB3D-AB42-077D-36A1-F355324E4237"
MOVIE_TYPES = {"movie", "movies", "film", "video"}
TV_TYPES = {"tv", "series", "tvshow", "tvshows", "show"}
FOLDER_TYPES = {"directory", "folder", "dir"}


def md5(text: str) -> str:
    return hashlib.md5(text.encode("utf-8")).hexdigest()


def normalize_base(url: str) -> str:
    value = (url or DEFAULT_BASE).strip().rstrip("/")
    if not value.startswith(("http://", "https://")):
        value = "http://" + value
    login_at = value.find("/v/login")
    if login_at >= 0:
        value = value[:login_at]
    if value.endswith("/v"):
        value = value[:-2]
    return value.rstrip("/")


def authx(path: str, body: str) -> str:
    nonce = str(random.randint(100000, 999999))
    timestamp = str(int(time.time() * 1000))
    sign = md5("_".join([API_KEY, path, nonce, timestamp, md5(body), API_SECRET]))
    return f"nonce={nonce}&timestamp={timestamp}&sign={sign}"


def request(base: str, method: str, path: str, token: str = "", payload=None, retries: int = 5):
    data_obj = payload
    if method in ("POST", "PUT"):
        data_obj = dict(payload or {})
        data_obj["nonce"] = str(random.randint(100000, 999999))
    body = json.dumps(data_obj, ensure_ascii=False, separators=(",", ":")) if data_obj is not None else ""
    raw = body.encode("utf-8") if body else None
    headers = {
        "Content-Type": "application/json",
        "Authorization": token,
        "Authx": authx(path, body),
    }
    last = None
    for _ in range(retries):
        req = urllib.request.Request(base + path, data=raw, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                content_type = resp.headers.get("Content-Type", "")
                blob = resp.read()
            if "application/json" not in content_type.lower():
                return blob
            parsed = json.loads(blob.decode("utf-8"))
            if parsed.get("code") == 5000 and parsed.get("msg") == "invalid sign":
                time.sleep(0.1)
                continue
            if parsed.get("code", 0) != 0:
                raise RuntimeError(parsed.get("msg") or f"code {parsed.get('code')}")
            return parsed.get("data")
        except Exception as ex:  # noqa: BLE001
            last = ex
            time.sleep(0.1)
    raise RuntimeError(str(last) if last else "request failed")


def as_list(data):
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        return data.get("list") or data.get("items") or []
    return []


def field_names(obj):
    if isinstance(obj, dict):
        return ",".join(sorted(obj.keys()))
    return ""


def guid_of(row):
    return str((row or {}).get("guid") or (row or {}).get("id") or "")


def title_of(row):
    return str((row or {}).get("title") or (row or {}).get("name") or (row or {}).get("tv_title") or guid_of(row))


def row_type(row):
    return str((row or {}).get("type") or "").strip().lower()


def summarize(rows):
    counts = {}
    for row in rows:
        key = row_type(row) or "unknown"
        counts[key] = counts.get(key, 0) + 1
    return ",".join(f"{key}={value}" for key, value in sorted(counts.items())) or "none"


def list_items(base: str, token: str, payload: dict):
    return as_list(request(base, "POST", "/v/api/v1/item/list", token, payload))


def pick_playable(base: str, token: str, rows, depth: int = 0):
    if depth > 3:
        return None
    for row in rows:
        if row_type(row) in MOVIE_TYPES and guid_of(row):
            return row
    for row in rows:
        if row_type(row) not in TV_TYPES or not guid_of(row):
            continue
        series_guid = guid_of(row)
        try:
            seasons = as_list(request(base, "GET", f"/v/api/v1/season/list/{series_guid}", token))
        except Exception:  # noqa: BLE001
            seasons = list_items(base, token, {
                "parent_guid": series_guid,
                "exclude_folder": 0,
                "sort_column": "sort_title",
                "sort_type": "ASC",
                "page": 1,
                "page_size": 20,
            })
        for season in seasons or [row]:
            season_guid = guid_of(season)
            if not season_guid:
                continue
            try:
                episodes = as_list(request(base, "GET", f"/v/api/v1/episode/list/{season_guid}", token))
            except Exception:  # noqa: BLE001
                episodes = []
            if episodes:
                return episodes[0]
        return row
    for row in rows:
        if row_type(row) not in FOLDER_TYPES or not guid_of(row):
            continue
        nested = list_items(base, token, {
            "parent_guid": guid_of(row),
            "exclude_folder": 0,
            "sort_column": "sort_title",
            "sort_type": "ASC",
            "page": 1,
            "page_size": 48,
        })
        found = pick_playable(base, token, nested, depth + 1)
        if found:
            return found
    for row in rows:
        if guid_of(row):
            return row
    return None


def main() -> int:
    print("Feiniu API smoke test. Password is not saved or logged.")
    base = normalize_base(input(f"Server [{DEFAULT_BASE}]: ").strip() or DEFAULT_BASE)
    username = input("Username: ").strip()
    password = getpass.getpass("Password: ")
    if not username or not password:
        print("username/password required", file=sys.stderr)
        return 2

    login = request(base, "POST", "/v/api/v1/login", "", {
        "app_name": "trimemedia-web",
        "username": username,
        "password": password,
    })
    token = login if isinstance(login, str) else (login or {}).get("token") or (login or {}).get("access_token")
    if not token:
        print("login did not return a token")
        return 1
    print("login: ok")

    libs = request(base, "GET", "/v/api/v1/mediadb/list", token)
    items = as_list(libs)
    print(f"libraries: {len(items)}")
    if not items:
        print("no media libraries")
        return 1

    lib = items[0]
    lib_name = lib.get("name") or lib.get("mdb_name") or lib.get("title") or "unnamed"
    print(f"library: {lib_name}")
    print(f"library_fields: {field_names(lib)}")
    parent = guid_of(lib)
    if not parent:
        print("library has no guid")
        return 1

    rows = list_items(base, token, {
        "ancestor_guid": parent,
        "tags": {"type": ["Movie", "TV", "Directory", "Video"]},
        "exclude_grouped_video": 1,
        "sort_column": "sort_title",
        "sort_type": "ASC",
        "page": 1,
        "page_size": 48,
    })
    mode = "ancestor_guid"
    if not rows:
        rows = list_items(base, token, {
            "parent_guid": parent,
            "exclude_folder": 0,
            "sort_column": "sort_title",
            "sort_type": "ASC",
            "page": 1,
            "page_size": 20,
        })
        mode = "parent_guid"
    print(f"items: {len(rows)} ({mode})")
    print(f"item_types: {summarize(rows)}")

    playable = pick_playable(base, token, rows)
    if not playable:
        print("no playable item")
        return 1
    item_guid = guid_of(playable)
    print(f"sample: {title_of(playable)}")
    print(f"sample_type: {row_type(playable) or 'unknown'}")

    info = request(base, "POST", "/v/api/v1/play/info", token, {"item_guid": item_guid})
    media_guid = (info or {}).get("media_guid") if isinstance(info, dict) else None
    print(f"media_guid: {'ok' if media_guid else 'missing'}")
    if media_guid:
        print(f"direct: {base}/v/api/v1/media/range/{media_guid}")

    streams = request(base, "GET", f"/v/api/v1/stream/list/{item_guid}", token) or {}
    if not isinstance(streams, dict):
        streams = {}
    subs = [s for s in streams.get("subtitle_streams") or [] if s.get("is_external")]
    print(f"external subtitles: {len(subs)}")
    if subs:
        sub = request(base, "GET", f"/v/api/v1/subtitle/dl/{subs[0].get('guid')}", token)
        size = len(sub) if isinstance(sub, (bytes, str)) else 0
        print(f"subtitle download: {size} bytes")
    print("smoke: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
