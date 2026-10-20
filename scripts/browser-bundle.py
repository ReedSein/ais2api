#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Stage or validate a clean browser bundle from the manifest-locked archive.

No network access. Example:
  python scripts/browser-bundle.py stage --archive cloakbrowser.zip --platform windows-x64 --output build/browser-bundle
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import tempfile
import zipfile

MARKER = '.ais2api-runtime.json'
PRIVATE = {'data', 'profiles', 'profile', 'default', 'user data', 'downloads', 'cookies',
           'network', 'local state', 'preferences', 'history', 'login data', 'web data',
           'sessions', 'cache', 'code cache', 'gpuCache'.lower(), 'logs', 'server-logs',
           '.git', '.env', 'config.yaml', 'installation.json'}

def safe_name(name):
    if '\\' in name or ':' in name or '\x00' in name:
        raise ValueError('unsafe_browser_path')
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError('unsafe_browser_path')
    for part in path.parts:
        lower = part.lower()
        device = lower.split('.')[0]
        if part.endswith(('.', ' ')) or any(ord(c) < 32 or c in '<>"|?*' for c in part) or device in {'con', 'prn', 'aux', 'nul', 'conin$', 'conout$'} or (len(device) == 4 and device[:3] in {'com', 'lpt'} and device[3] in '123456789¹²³'):
            raise ValueError('unsafe_windows_browser_path: ' + name)
        if lower in PRIVATE or lower.endswith(('.sqlite', '.sqlite3', '.db', '.log', '.dmp')):
            raise ValueError('private_browser_path: ' + name)
    return path.as_posix()

def digest(source):
    result = hashlib.sha256()
    while block := source.read(1024 * 1024):
        result.update(block)
    return result.hexdigest()

def manifest_identity(manifest, platform):
    value = json.loads(Path(manifest).read_text(encoding='utf-8'))
    return {'platform': platform, 'browser_version': value['browser_version'],
            'sha256': value['artifacts'][platform]['sha256']}

def validate(directory, manifest, platform):
    root = Path(directory)
    if root.is_symlink() or not root.is_dir():
        raise ValueError('browser_bundle_not_regular_directory')
    marker_path = root / MARKER
    if marker_path.is_symlink() or getattr(marker_path.stat(), 'st_file_attributes', 0) & 0x400:
        raise ValueError('browser_bundle_marker_link')
    marker = json.loads(marker_path.read_text(encoding='utf-8'))
    identity = manifest_identity(manifest, platform)
    if any(marker.get(key) != value for key, value in identity.items()):
        raise ValueError('browser_bundle_manifest_mismatch')
    files = marker.get('files')
    if not isinstance(files, dict) or not files:
        raise ValueError('browser_bundle_missing_inventory_use_stage_command')
    actual = {}
    for item in root.rglob('*'):
        if item.is_symlink() or getattr(item.stat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('browser_bundle_link')
        relative = safe_name(item.relative_to(root).as_posix())
        if item.is_file() and relative != MARKER:
            with item.open('rb') as source:
                actual[relative] = digest(source)
        elif not item.is_file() and not item.is_dir():
            raise ValueError('browser_bundle_special_file')
    if actual != files:
        raise ValueError('browser_bundle_inventory_mismatch')
    executable = 'chrome.exe' if platform == 'windows-x64' else 'chrome'
    if 'BINARY-LICENSE.md' not in files or marker.get('usage') != 'local-internal':
        raise ValueError('browser_bundle_missing_binary_license_or_usage')
    if executable not in files:
        raise ValueError('browser_bundle_missing_executable')
    return marker

def stage(archive_path, output, manifest, platform, license_file):
    identity = manifest_identity(manifest, platform)
    with Path(archive_path).open('rb') as source:
        if digest(source) != identity['sha256']:
            raise ValueError('browser_archive_checksum_mismatch')
    output = Path(output).absolute()
    if output.exists():
        raise ValueError('browser_bundle_output_already_exists')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.browser-stage-', dir=output.parent) as temporary:
        temporary = Path(temporary)
        count = 0
        expanded = 0
        def bounded(size):
            nonlocal count, expanded
            count += 1
            expanded += size
            if count > 100000 or expanded > 4 * 1024 ** 3:
                raise ValueError('browser_archive_expansion_limit')
        if zipfile.is_zipfile(archive_path):
            with zipfile.ZipFile(archive_path) as archive:
                for entry in archive.infolist():
                    bounded(entry.file_size)
                    name = safe_name(entry.filename)
                    mode = entry.external_attr >> 16
                    if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) and not (stat.S_ISDIR(mode) or stat.S_ISREG(mode))):
                        raise ValueError('browser_archive_special_file')
                    target = temporary / name
                    if entry.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with archive.open(entry) as source, target.open('xb') as destination:
                            shutil.copyfileobj(source, destination)
        else:
            with tarfile.open(archive_path, 'r:*') as archive:
                for entry in archive:
                    bounded(entry.size)
                    name = safe_name(entry.name)
                    target = temporary / name
                    if entry.isdir():
                        target.mkdir(parents=True, exist_ok=True)
                    elif entry.isfile():
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with archive.extractfile(entry) as source, target.open('xb') as destination:
                            shutil.copyfileobj(source, destination)
                        target.chmod(entry.mode & 0o777)
                    else:
                        raise ValueError('browser_archive_special_file')
        executable = 'chrome.exe' if platform == 'windows-x64' else 'chrome'
        candidates = list(temporary.rglob(executable))
        if len(candidates) != 1:
            raise ValueError('browser_archive_ambiguous_executable')
        root = candidates[0].parent
        if any(item.is_file() and not item.is_relative_to(root) for item in temporary.rglob('*')):
            raise ValueError('browser_archive_files_outside_browser_directory')
        license_path = root / 'BINARY-LICENSE.md'
        if not license_path.exists():
            shutil.copyfile(license_file, license_path)
        files = {}
        for item in root.rglob('*'):
            if item.is_file():
                with item.open('rb') as source:
                    files[item.relative_to(root).as_posix()] = digest(source)
        if MARKER in files:
            raise ValueError('browser_archive_unexpected_marker')
        (root / MARKER).write_text(json.dumps({**identity, 'usage': 'local-internal', 'files': files}, sort_keys=True, indent=2) + '\n', encoding='utf-8')
        validate(root, manifest, platform)
        shutil.move(str(root), str(output))
    print('Verified browser bundle: ' + str(output))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['stage', 'validate'])
    parser.add_argument('--manifest', default=str(Path(__file__).resolve().parent.parent / 'runtime-manifest.json'))
    parser.add_argument('--platform', choices=['windows-x64', 'linux-x64', 'termux-arm64'], required=True)
    parser.add_argument('--archive')
    parser.add_argument('--license-file', help='Local copy of the applicable official CloakBrowser BINARY-LICENSE.md')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.command == 'stage':
        if not args.archive or not args.license_file:
            parser.error('stage requires --archive and --license-file')
        stage(args.archive, args.output, args.manifest, args.platform, args.license_file)
    else:
        validate(args.output, args.manifest, args.platform)
        print('Browser bundle inventory verified')
