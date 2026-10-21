#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""Check runtime archives without extraction or service launch."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import stat
import sys
import tarfile
import zipfile

spec = importlib.util.spec_from_file_location('browser_bundle', Path(__file__).with_name('browser-bundle.py'))
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)

def check(path: Path, with_browser=False) -> int:
    archive = zipfile.ZipFile(path) if path.suffix == '.zip' else tarfile.open(path, 'r:gz')
    with archive:
        names = {}
        entries = archive.infolist() if isinstance(archive, zipfile.ZipFile) else archive.getmembers()
        for entry in entries:
            entry_name = entry.filename if isinstance(archive, zipfile.ZipFile) else entry.name
            entry_path = PurePosixPath(entry_name)
            if '\\' in entry_name or ':' in entry_name or entry_path.is_absolute() or '..' in entry_path.parts:
                raise ValueError('unsafe_archive_entry')
            if isinstance(archive, zipfile.ZipFile):
                mode = entry.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise ValueError('archive_contains_links')
                if entry.is_dir():
                    continue
                name = entry.filename
            else:
                if not entry.isfile() and not entry.isdir():
                    raise ValueError('archive_contains_special_files')
                if entry.isdir():
                    continue
                name = entry.name
            if name in names:
                raise ValueError('duplicate_archive_member')
            names[name] = entry
        def read(name, limit=None):
            source = archive.open(names[name]) if isinstance(archive, zipfile.ZipFile) else archive.extractfile(names[name])
            with source:
                return source.read() if limit is None else source.read(limit)
        licenses = [name for name in names if name.endswith('/share/doc/ais2api/LICENSE')]
        if len(licenses) != 1 or not read(licenses[0], 100000).startswith(b'Mozilla Public License Version 2.0'):
            raise ValueError('project_license_is_not_MPL_2_0')
        forbidden = {'.tools', '.analysis', '.git', 'vcpkg_installed', 'data', 'dumps', 'server-logs',
                     '.env', 'installation.json', 'config.yaml', 'config.json', 'config.local.json',
                     'credentials', 'secrets', 'cookies.json', 'cookies.txt', 'storage_state.json',
                     'storage-state.json', 'service.log', 'state.sqlite3'}
        relative, roots, original = set(), set(), {}
        for name in names:
            member = PurePosixPath(name)
            if '\\' in name or ':' in name or member.is_absolute() or '..' in member.parts or len(member.parts) < 2:
                raise ValueError('unsafe_or_unrooted_archive_entry')
            if forbidden.intersection(member.parts):
                raise ValueError('private_or_build_files_in_archive')
            roots.add(member.parts[0])
            item = PurePosixPath(*member.parts[1:]).as_posix()
            if item in relative:
                raise ValueError('duplicate_normalized_archive_member')
            relative.add(item)
            original[item] = name
        if len(roots) != 1:
            raise ValueError('archive_must_have_one_root')
        executable = 'bin/ais2api.exe' if path.suffix == '.zip' else 'bin/ais2api'
        required = {executable, 'bin/browser/bridge.js', 'bin/runtime-manifest.json', 'bin/userscript/ais2api-cookie.user.js', 'share/doc/ais2api/LICENSE', 'share/doc/ais2api/platforms.md', 'share/doc/ais2api/NOTICE', 'share/doc/ais2api/THIRD_PARTY_NOTICES.md'}
        if not required.issubset(relative):
            raise ValueError('missing_runtime_assets')
        if not any(name.startswith('share/ais2api/schemas/') for name in relative):
            raise ValueError('missing_schemas')
        if not any(name.startswith('share/licenses/ais2api-dependencies/') and name.endswith('/copyright') for name in relative):
            raise ValueError('missing_dependency_licenses')
        browser_files = {name for name in relative if name.startswith('bin/browser-runtime/')}
        if with_browser:
            platform = 'windows-x64' if path.suffix == '.zip' else 'linux-x64'
            prefix = 'bin/browser-runtime/' + platform + '/'
            manifest = json.loads(read(original['bin/runtime-manifest.json'], 100000))
            marker_name = prefix + bundle.MARKER
            if marker_name not in relative:
                raise ValueError('missing_browser_bundle_marker')
            marker = json.loads(read(original[marker_name], 4000000))
            expected = {'platform': platform, 'browser_version': manifest['browser_version'], 'sha256': manifest['artifacts'][platform]['sha256']}
            if any(marker.get(key) != value for key, value in expected.items()):
                raise ValueError('browser_bundle_manifest_mismatch')
            actual = {}
            # Compressed tar seeks backwards by replaying decompression. Hash in
            # archive order so a large browser is read sequentially.
            ordered_browser_files = sorted(browser_files, key=lambda name:
                names[original[name]].header_offset if isinstance(archive, zipfile.ZipFile)
                else names[original[name]].offset_data)
            for name in ordered_browser_files:
                if not name.startswith(prefix):
                    raise ValueError('unexpected_browser_platform')
                local = bundle.safe_name(name[len(prefix):])
                if local == bundle.MARKER:
                    continue
                source = archive.open(names[original[name]]) if isinstance(archive, zipfile.ZipFile) else archive.extractfile(names[original[name]])
                with source:
                    actual[local] = bundle.digest(source)
            if 'BINARY-LICENSE.md' not in actual or marker.get('usage') != 'local-internal':
                raise ValueError('missing_browser_binary_license_or_local_usage')
            if not actual or marker.get('files') != actual:
                raise ValueError('browser_bundle_inventory_mismatch')
            if ('chrome.exe' if platform == 'windows-x64' else 'chrome') not in actual:
                raise ValueError('missing_browser_executable')
        elif browser_files:
            raise ValueError('unexpected_browser_bundle_use_with_browser')
    expected = Path(str(path) + '.sha256').read_text(encoding='utf-8').split()[0].lower()
    with path.open('rb') as source:
        digest = bundle.digest(source)
    if digest != expected:
        raise ValueError('archive_checksum_mismatch')
    print(f'Runtime archive verified: {len(names)} files, SHA-256 matches' + (', browser inventory matches' if with_browser else ''))
    return 0

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--with-browser', action='store_true')
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    try:
        sys.exit(check(args.archive, args.with_browser))
    except Exception as error:
        print(f'Runtime archive validation failed: {type(error).__name__}: {error}', file=sys.stderr)
        sys.exit(1)
