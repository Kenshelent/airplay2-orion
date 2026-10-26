#!/usr/bin/env python3
"""Build airplay2-orion-1.0.0.yap (tar.gz with plugin.toml at root)."""
from __future__ import annotations

import hashlib
import json
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
YAM = Path(__file__).resolve().parent
PAYLOAD = ROOT / "ap2-orion-aarch64.tar.gz"
OUT = ROOT / "airplay2-orion-1.0.0.yap"
INDEX = ROOT / "yam-registry" / "index.json"

SKIP_PREFIX = ("yam/", "src/", ".git/", "yam-registry/")
SKIP_NAMES = {
    "pack_yap.py",
    "airplay2-orion-1.0.0.yap",
    "ap2-orion-aarch64.tar.gz",
}


def add_bytes(archive: tarfile.TarFile, name: str, data: bytes, mode: int) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    archive.addfile(info, __import__("io").BytesIO(data))


def main() -> None:
    if not PAYLOAD.is_file():
        raise SystemExit(f"missing payload: {PAYLOAD}")

    if OUT.exists():
        OUT.unlink()

    with tarfile.open(OUT, "w:gz", format=tarfile.GNU_FORMAT, compresslevel=6) as archive:
        add_bytes(archive, "plugin.toml", (YAM / "plugin.toml").read_bytes(), 0o644)
        add_bytes(archive, "launch.sh", (YAM / "launch.sh").read_bytes().replace(b"\r\n", b"\n"), 0o755)

        overlay_airplay = (ROOT / "airplay.sh").read_bytes().replace(b"\r\n", b"\n")
        overlay_daemon = (ROOT / "airplayd.bin").read_bytes() if (ROOT / "airplayd.bin").is_file() else None

        seen = {"plugin.toml", "launch.sh"}
        with tarfile.open(PAYLOAD, "r:gz") as src:
            for member in src.getmembers():
                name = member.name.lstrip("./")
                if not name or member.isdir():
                    continue
                if name in seen:
                    continue
                if member.issym() or member.islnk() or member.isdev():
                    continue
                data = src.extractfile(member)
                if data is None:
                    continue
                blob = data.read()
                if name == "airplay.sh":
                    blob = overlay_airplay
                elif name == "airplayd" and overlay_daemon is not None:
                    blob = overlay_daemon
                mode = 0o755 if name in {"airplay.sh", "airplayd"} or "/bin/" in name or name.endswith(
                    ("shairport-sync-ap2", "nqptp", "avahi-daemon", "avahi-publish")
                ) else (member.mode or 0o644)
                add_bytes(archive, name, blob, mode)
                seen.add(name)

        if "airplay.sh" not in seen:
            add_bytes(archive, "airplay.sh", overlay_airplay, 0o755)
        if overlay_daemon is not None and "airplayd" not in seen:
            add_bytes(archive, "airplayd", overlay_daemon, 0o755)

    digest = hashlib.sha256(OUT.read_bytes()).hexdigest()
    INDEX.parent.mkdir(parents=True, exist_ok=True)
    INDEX.write_text(
        json.dumps(
            {
                "format": 2,
                "packages": [
                    {
                        "id": "airplay2-orion",
                        "version": "1.0.0",
                        "name": "AirPlay 2 (Станция 3)",
                        "description": "AirPlay 2 для Яндекс Станции 3 (Orion). Алиса остаётся включённой.",
                        "sha256": digest,
                        "url": "https://github.com/Kenshelent/airplay2-orion/releases/download/v1.0.0/airplay2-orion-1.0.0.yap",
                    }
                ],
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(OUT)
    print("sha256", digest)
    print("size", OUT.stat().st_size)


if __name__ == "__main__":
    main()
