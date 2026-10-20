#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Isolated CLI/IPC/HTTP smoke checks; Python is not a runtime dependency.

No browser preparation or Google request is performed. Authentication is tested
with an intentionally unsupported Content-Type, before generation is enqueued.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


class SmokeFailure(Exception):
    """A label-only failure, never a child output or secret-bearing exception."""


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


class Smoke:
    def __init__(self, executable: Path, workspace: Path) -> None:
        self.executable = executable.resolve()
        workspace = workspace.resolve()
        tools = workspace / ".tools"
        if tools.is_symlink() or tools.resolve().parent != workspace:
            raise SmokeFailure("unsafe_fixture_root")
        tools.mkdir(exist_ok=True)
        self.fixture_root = tools.resolve()
        self.directory = Path(tempfile.mkdtemp(prefix="smoke-", dir=tools)).resolve()
        self.config = self.directory / "config.json"
        self.data = self.directory / "data"
        self.environment = {
            key: value for key, value in os.environ.items()
            if not key.upper().startswith("AIS2API_")
        }
        self.checks: list[str] = []
        self.current = "initialize"
        self.secrets: list[str] = []
        self.canary = "synthetic-cookie-canary-not-a-real-credential"
        self.proxy_user = "synthetic-proxy-user"
        self.proxy_password = "synthetic-proxy-canary"
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.port = 0
        self.started = False

    def check(self, name: str, condition: bool) -> None:
        self.current = name
        if not condition:
            raise SmokeFailure(name)
        self.checks.append(name)

    def no_leak(self, text: str, *, proxy: bool = False) -> None:
        values = [self.canary, *self.secrets]
        if proxy:
            values += [self.proxy_user, self.proxy_password]
        if any(value and value in text for value in values):
            raise SmokeFailure("credential_redaction")

    def cli(self, *arguments: str, stdin: str | None = None) -> dict:
        # Capture everything in memory. Never include child output in an error.
        process = subprocess.Popen(
            [str(self.executable), "--config", str(self.config),
             "--data-dir", str(self.data), *arguments],
            stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="strict",
            env=self.environment, cwd=self.directory,
        )
        try:
            stdout, stderr = process.communicate(input=stdin, timeout=15)
        except subprocess.TimeoutExpired:
            # subprocess.run performs a second, unbounded communicate on Windows.
            # If a background child incorrectly inherits the capture pipes that
            # would hang this harness before its finally block can stop service.
            try:
                process.kill()
            except OSError:
                pass
            process.wait(timeout=5)
            raise
        self.no_leak(stderr)
        if process.returncode:
            raise SmokeFailure(self.current)
        self.no_leak(stdout)
        try:
            result = json.loads(stdout)
        except (ValueError, TypeError):
            raise SmokeFailure("machine_readable_cli") from None
        if not isinstance(result, dict):
            raise SmokeFailure("machine_readable_cli")
        return result

    def http(self, path: str, key: str | None = None, *, post: bool = False) -> tuple[int, dict]:
        headers = {"Authorization": "Bearer " + key} if key else {}
        if post:
            headers["Content-Type"] = "text/plain"
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=b"" if post else None, headers=headers,
        )
        try:
            with self.opener.open(request, timeout=15) as response:
                status, body = response.status, response.read(65536)
        except urllib.error.HTTPError as error:
            status, body = error.code, error.read(65536)
        text = body.decode("utf-8")
        self.no_leak(text)
        result = json.loads(text)
        if not isinstance(result, dict):
            raise SmokeFailure("machine_readable_http")
        return status, result

    def auth_probe(self, key: str | None) -> int:
        # Recognized route -> authentication -> Content-Type validation. A valid
        # key gets 415 without enqueueing work or touching Chromium/Google.
        return self.http("/v1/chat/completions", key, post=True)[0]

    def run(self) -> None:
        self.current = "initial_key_once"
        first = self.cli("init")
        initial = first.get("api_key", {}).get("secret")
        self.check("initial_key_created", isinstance(initial, str) and initial.startswith("sk-ais2api-"))
        self.secrets.append(initial)
        second = self.cli("init")
        self.check("initial_key_not_reissued", "api_key" not in second)

        configuration = json.loads(self.config.read_text(encoding="utf-8"))
        configured_port = free_port()
        self.port = free_port()
        while self.port == configured_port:
            self.port = free_port()
        configuration.update({
            "port": configured_port,
            "bind": "127.0.0.1",
            "data_dir": str(self.data),
            "browser_executable": str(self.directory / "browser-must-not-run"),
            "proxy": f"http://{self.proxy_user}:{self.proxy_password}@127.0.0.1:9",
        })
        configuration["limits"]["queue_size"] = 4
        self.config.write_text(json.dumps(configuration), encoding="utf-8")

        self.current = "start_effective_override"
        self.started = True  # Even a start timeout must trigger finally cleanup.
        status = self.cli("start", "--port", str(self.port))
        self.check("start_reports_running", status.get("running") is True)
        code, health = self.http("/health")
        self.check("start_port_override_reaches_http", code == 200 and health.get("status") == "alive")

        self.current = "online_account_import"
        imported = self.cli("accounts", "import", "--stdin", "--name", "Synthetic smoke account",
                            stdin=f"SAPISID={self.canary}; SID=synthetic-only\r\n")
        self.check("online_cookie_import", imported.get("imported") == 1)
        accounts = self.cli("accounts", "list").get("accounts", [])
        self.check("online_account_list", len(accounts) == 1 and accounts[0].get("name") == "Synthetic smoke account")
        self.check("online_account_outputs_redacted", self.canary not in json.dumps([imported, accounts]))

        self.current = "http_authentication"
        self.check("missing_key_rejected", self.auth_probe(None) == 401)
        self.check("initial_key_accepted_before_generation", self.auth_probe(initial) == 415)
        created = self.cli("keys", "create", "Synthetic smoke key")
        secret, key_id = created.get("secret"), created.get("id")
        self.check("online_key_create", isinstance(secret, str) and isinstance(key_id, str))
        self.secrets.append(secret)
        self.check("new_key_accepted_before_generation", self.auth_probe(secret) == 415)
        keys = self.cli("keys", "list").get("keys", [])
        self.check("key_list_has_metadata_only", any(key.get("id") == key_id for key in keys)
                   and all("secret" not in key and "hash" not in key for key in keys))
        self.cli("keys", "revoke", key_id)
        self.check("revoked_key_rejected", self.auth_probe(secret) == 401)
        remaining = self.cli("keys", "list").get("keys", [])
        self.check("revoked_key_removed_from_list", all(key.get("id") != key_id for key in remaining))
        self.check("initial_key_remains_accepted", self.auth_probe(initial) == 415)

        self.current = "configuration_redaction_reload"
        shown = self.cli("config", "show")
        self.no_leak(json.dumps(shown), proxy=True)
        self.check("proxy_userinfo_redacted", "***@127.0.0.1:9" in shown.get("config", {}).get("proxy", ""))
        configuration["limits"]["queue_size"] = 2
        self.config.write_text(json.dumps(configuration), encoding="utf-8")
        self.check("config_validation", self.cli("config", "validate").get("valid") is True)
        reloaded = self.cli("config", "reload")
        self.check("config_reload", reloaded.get("status") == "reloaded")
        current = self.cli("config", "show").get("config", {})
        self.check("queue_size_reloaded", current.get("limits", {}).get("queue_size") == 2)
        self.check("port_override_survives_reload", current.get("port") == self.port)
        self.current = "online_stats"
        self.check("online_stats", isinstance(self.cli("stats"), dict))
        log = self.data / "service.log"
        if log.exists():
            self.no_leak(log.read_text(encoding="utf-8", errors="replace"), proxy=True)
        self.check("service_log_has_no_fixture_secrets", True)
        self.current = "stop_and_offline_status"
        self.cli("stop")
        self.wait_offline()
        self.check("stop_reports_offline", self.cli("status").get("running") is False)
        self.started = False

    def wait_offline(self) -> None:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                if self.cli("status").get("running") is False:
                    # An offline management command must acquire InstanceLock.
                    # This proves the old service released database ownership.
                    if self.cli("config", "validate").get("valid") is True:
                        return
            except (SmokeFailure, subprocess.TimeoutExpired):
                pass
            time.sleep(0.2)
        raise SmokeFailure("service_exit_timeout")

    def cleanup(self) -> bool:
        try:
            if self.started:
                try:
                    self.cli("stop")
                except (SmokeFailure, subprocess.TimeoutExpired):
                    pass
                self.wait_offline()
            # Resolve and verify the exact test-owned subtree before removal.
            target = self.directory.resolve()
            if target.parent != self.fixture_root or not target.name.startswith("smoke-"):
                return False
            shutil.rmtree(target)
            return True
        except Exception:
            # Keep owned files if the service cannot be proven stopped.
            return False


def main() -> int:
    workspace = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=workspace / "build/windows/ais2api.exe")
    args = parser.parse_args()
    smoke = None
    failure = None
    try:
        smoke = Smoke(args.exe, workspace)
        smoke.run()
    except Exception:
        # Do not print exception text, child output, headers, secrets or traces.
        failure = smoke.current if smoke else "initialize"
    finally:
        clean = smoke.cleanup() if smoke else True
    if not clean:
        failure = "cleanup_not_confirmed"
    checks = smoke.checks if smoke else []
    report = {"ok": failure is None, "checks": checks,
              "count": len(checks), "cleanup": clean}
    if failure:
        report["failed"] = failure
    print(json.dumps(report, separators=(",", ":")))
    return int(failure is not None)


if __name__ == "__main__":
    sys.exit(main())
