#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Check runtime archive contents without extracting or launching a service."""
from __future__ import annotations

import hashlib
from pathlib import Path, PurePosixPath
import sys
import tarfile
import zipfile


def check(path: Path) -> int:
    license_text = b""
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
            licenses = [name for name in names if name.endswith("/share/doc/ais2api/LICENSE")]
            if len(licenses) == 1:
                with archive.open(licenses[0]) as source:
                    license_text = source.read(100000)
    else:
        with tarfile.open(path, "r:gz") as archive:
            names = [entry.name for entry in archive.getmembers() if entry.isfile()]
            licenses = [name for name in names if name.endswith("/share/doc/ais2api/LICENSE")]
            if len(licenses) == 1:
                with archive.extractfile(licenses[0]) as source:
                    license_text = source.read(100000)
    if not license_text.startswith(b"Mozilla Public License Version 2.0"):
        raise ValueError("project_license_is_not_MPL_2_0")
    forbidden = {".tools", ".analysis", ".git", "vcpkg_installed", "data", "dumps", "server-logs", ".env", "installation.json", "config.yaml", "state.sqlite3"}
    relative = set()
    roots = set()
    for name in names:
        member = PurePosixPath(name.replace("\\", "/"))
        if member.is_absolute() or ".." in member.parts or len(member.parts) < 2:
            raise ValueError("unsafe_or_unrooted_archive_entry")
        if forbidden.intersection(member.parts):
            raise ValueError("private_or_build_files_in_archive")
        roots.add(member.parts[0])
        relative.add(PurePosixPath(*member.parts[1:]).as_posix())
    if len(roots) != 1:
        raise ValueError("archive_must_have_one_root")
    executable = "bin/ais2api.exe" if path.suffix == ".zip" else "bin/ais2api"
    required = {executable, "bin/browser/bridge.js", "bin/runtime-manifest.json", "share/doc/ais2api/LICENSE", "share/doc/ais2api/platforms.md"}
    required.update({"share/doc/ais2api/NOTICE", "share/doc/ais2api/THIRD_PARTY_NOTICES.md"})
    if not required.issubset(relative):
        raise ValueError("missing_runtime_assets")
    if not any(name.startswith("share/ais2api/schemas/") for name in relative):
        raise ValueError("missing_schemas")
    if not any(name.startswith("share/licenses/ais2api-dependencies/") and name.endswith("/copyright") for name in relative):
        raise ValueError("missing_dependency_licenses")
    sidecar = Path(str(path) + ".sha256")
    expected = sidecar.read_text(encoding="utf-8").split()[0].lower()
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != expected:
        raise ValueError("archive_checksum_mismatch")
    print(f"Runtime archive verified: {len(names)} files, SHA-256 matches")
    return 0


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("provide_one_runtime_archive")
        sys.exit(check(Path(sys.argv[1])))
    except Exception as error:
        print(f"Runtime archive validation failed: {type(error).__name__}", file=sys.stderr)
        sys.exit(1)
