#!/usr/bin/env python3
"""Generate the one supported FBI QR and refuse unverified download URLs."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path


DEFAULT_REPO = "baldicoot123/bilibili-3ds"
PREFIX_SIZE = 64 * 1024


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def request(opener, url: str, method: str, headers: dict[str, str] | None = None):
    req_headers = {"User-Agent": "bilibili-3ds-release-validator/1.0"}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, method=method, headers=req_headers)
    try:
        return opener.open(req, timeout=45)
    except urllib.error.HTTPError as exc:
        if 300 <= exc.code < 400:
            raise RuntimeError(
                f"URL redirected ({exc.code} -> {exc.headers.get('Location', '?')}); "
                "FBI QR must use a zero-redirect URL"
            ) from exc
        raise RuntimeError(f"HTTP request failed: {exc.code} {exc.reason}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"network request failed: {exc.reason}") from exc


def validate_download(url: str, cia: Path) -> tuple[int, str]:
    size = cia.stat().st_size
    digest = hashlib.sha256(cia.read_bytes()).hexdigest()
    opener = urllib.request.build_opener(NoRedirect)

    with request(opener, url, "HEAD") as response:
        if response.status != 200:
            raise RuntimeError(f"HEAD status is {response.status}, expected 200")
        remote_size = int(response.headers.get("Content-Length", "0"))
        if remote_size != size:
            raise RuntimeError(
                f"Content-Length mismatch: remote={remote_size}, local={size}"
            )
        if response.headers.get("Accept-Ranges", "").lower() != "bytes":
            raise RuntimeError("server does not advertise Accept-Ranges: bytes")

    last = min(PREFIX_SIZE, size) - 1
    with request(opener, url, "GET", {"Range": f"bytes=0-{last}"}) as response:
        if response.status != 206:
            raise RuntimeError(f"Range status is {response.status}, expected 206")
        expected_range = f"bytes 0-{last}/{size}"
        actual_range = response.headers.get("Content-Range", "")
        if actual_range != expected_range:
            raise RuntimeError(
                f"Content-Range mismatch: {actual_range!r}, expected {expected_range!r}"
            )
        remote_prefix = response.read(PREFIX_SIZE + 1)

    with cia.open("rb") as stream:
        local_prefix = stream.read(PREFIX_SIZE)
    if remote_prefix != local_prefix:
        raise RuntimeError("remote first 64 KiB does not match the local CIA")
    return size, digest


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate the stable CIA URL, then generate its FBI QR PNG."
    )
    parser.add_argument("version", help="release version, for example v1.8.0")
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub owner/repo")
    parser.add_argument("--cia", type=Path, help="local CIA built by Actions")
    parser.add_argument("--output", type=Path, help="PNG path under downloads/")
    args = parser.parse_args()

    match = re.fullmatch(r"v?(\d+\.\d+\.\d+)", args.version)
    if not match:
        parser.error("version must look like v1.8.0")
    version = match.group(1)
    cia = args.cia or Path("release") / f"v{version}" / "bilibili.cia"
    output = args.output or Path("downloads") / f"bilibili-v{version}-fbi-qr.png"
    if not cia.is_file():
        parser.error(f"CIA not found: {cia}")

    cia_url = (
        f"https://quantil.jsdelivr.net/gh/{args.repo}@main/"
        f"downloads/bilibili-v{version}.cia"
    )
    try:
        size, digest = validate_download(cia_url, cia)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        print("QR was not generated.", file=sys.stderr)
        return 1

    try:
        import qrcode
    except ImportError:
        print("ERROR: install the host tool with: pip install 'qrcode[pil]'", file=sys.stderr)
        return 1

    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=12,
        border=4,
    )
    qr.add_data(cia_url)
    qr.make(fit=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    qr.make_image(fill_color="black", back_color="white").save(output)

    raw_url = f"https://raw.githubusercontent.com/{args.repo}/main/{output.as_posix()}"
    print("FBI QR validation passed")
    print(f"  HTTP: 200 HEAD, 206 Range, zero redirects")
    print(f"  Size: {size} bytes")
    print(f"  SHA-256: {digest}")
    print(f"  CIA URL: {cia_url}")
    print(f"  PNG: {output}")
    print(f"  Display URL after committing PNG: {raw_url}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
