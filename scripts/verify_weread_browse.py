#!/usr/bin/env python3
"""Verify WeRead Cookie-only browse endpoints without printing account data."""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

MAX_RESPONSE_BYTES = 4 * 1024 * 1024
ORIGIN = "https://weread.qq.com"
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/135 Safari/537.36"


def request_json(path: str, cookie: str) -> tuple[int, object]:
    request = urllib.request.Request(
        ORIGIN + path,
        headers={"Accept": "application/json, text/plain, */*", "Cookie": cookie,
                 "Origin": ORIGIN, "Referer": ORIGIN + "/", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            payload, status = response.read(MAX_RESPONSE_BYTES + 1), response.status
    except urllib.error.HTTPError as error:
        payload, status = error.read(MAX_RESPONSE_BYTES + 1), error.code
    except (urllib.error.URLError, TimeoutError):
        return 0, {}
    if len(payload) > MAX_RESPONSE_BYTES:
        return status, {}
    try:
        return status, json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return status, {}


def has_any(item: object, *fields: str) -> bool:
    return isinstance(item, dict) and any(field in item for field in fields)


def list_or_empty(payload: object, field: str) -> tuple[list[object], bool]:
    if not isinstance(payload, dict):
        return [], False
    if not payload:
        return [], True
    items = payload.get(field)
    return (items, True) if isinstance(items, list) else ([], False)


def unwrap_review(item: object) -> object:
    for _ in range(4):
        if not isinstance(item, dict) or not isinstance(item.get("review"), dict):
            break
        item = item["review"]
    return item


def result(name: str, status: int, items: list[object], checks: dict[str, bool]) -> bool:
    fields = " ".join(f"{key}={'yes' if value else 'no'}" for key, value in checks.items())
    print(f"{name}: status={status} count={len(items)} {fields}")
    return status == 200 and all(checks.values())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("book_id", nargs="?", default=os.environ.get("WEREAD_BOOK_ID"))
    args = parser.parse_args()
    cookie = os.environ.get("WEREAD_COOKIE")
    if not cookie or not args.book_id:
        print("Set WEREAD_COOKIE and pass book_id (or set WEREAD_BOOK_ID).", file=sys.stderr)
        return 2
    book_id = urllib.parse.quote(args.book_id, safe="")
    ok = True

    status, payload = request_json(f"/web/book/bestbookmarks?bookId={book_id}", cookie)
    popular = payload.get("bestBookMarks", payload) if isinstance(payload, dict) else payload
    items, valid = list_or_empty(popular, "items")
    sample = items[0] if items else {}
    ok &= result("popular_highlights", status, items,
                 {"list": valid, "text": not items or has_any(sample, "markText"),
                  "chapter": not items or has_any(sample, "chapterUid")})

    status, payload = request_json(f"/web/book/bookmarklist?bookId={book_id}", cookie)
    items, valid = list_or_empty(payload, "updated")
    sample = items[0] if items else {}
    ok &= result("my_highlights", status, items,
                 {"list": valid, "text": not items or has_any(sample, "markText"),
                  "chapter": not items or has_any(sample, "chapterUid")})

    path = f"/web/review/list?bookId={book_id}&listType=3&listMode=2&maxIdx=0&count=20&synckey=0"
    status, payload = request_json(path, cookie)
    items, valid = list_or_empty(payload, "reviews")
    sample = unwrap_review(items[0]) if items else {}
    empty = isinstance(payload, dict) and not payload
    ok &= result("popular_reviews", status, items,
                 {"list": valid, "text": not items or has_any(sample, "content", "htmlContent"),
                  "cursor": empty or (isinstance(payload, dict) and "reviewsHasMore" in payload and "synckey" in payload)})
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
