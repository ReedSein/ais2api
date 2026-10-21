# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright 2026 Mana Nekoha (mananekoha114)
"""Run the original OpenAI request converter with offline synthetic input.

Input/output are JSON arrays. Offline audit violations abort the process rather
than impersonating an upstream validation error. Remote media is intentionally
outside this probe's domain; inline media uses the original converter unchanged.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    if not args.source.is_absolute():
        parser.error("--source must be an absolute original checkout path")
    source = args.source.resolve(strict=True)
    if not (source / "src/aistudio_api/application/openai_request.py").is_file():
        parser.error("--source does not contain the upstream converter")

    workspace = Path(__file__).resolve().parents[1]
    sandbox = workspace / "build/llm-upstream-probe"
    sandbox.mkdir(parents=True, exist_ok=True)
    os.chdir(sandbox)
    sys.dont_write_bytecode = True
    # Upstream imports config, diagnostics and runtime state. Use its supported
    # environment controls to keep those side effects in this private sandbox.
    os.environ.update({
        "PYTHON_DOTENV_DISABLED": "1",
        "AISTUDIO_SILENT": "1",
        "AISTUDIO_LOG_DIR": str(sandbox / "logs"),
        "AISTUDIO_DATA_DIR": str(sandbox / "data"),
        "AISTUDIO_CONFIG_FILE": str(sandbox / "config.yaml"),
        "AISTUDIO_AUTH_FILE": str(sandbox / "absent-synthetic-auth.json"),
        "AISTUDIO_STATS_FILE": str(sandbox / "stats.json"),
        "AISTUDIO_ROTATOR_STATE_FILE": str(sandbox / "rotator.json"),
        "AISTUDIO_PERSIST_STATS": "0",
        "AISTUDIO_PERSIST_ROTATOR": "0",
        # A configured inert proxy prevents upstream's import-time local proxy
        # autodetection from attempting socket connections.
        "AISTUDIO_PROXY": "http://127.0.0.1:9",
        "AISTUDIO_TMP_DIR": str(sandbox),
        "AISTUDIO_DUMP_DIR": str(sandbox / "dumps"),
    })
    sys.path.insert(0, str(source / "src"))

    # Windows event-loop construction itself uses a local socket pair. Create
    # only that infrastructure before installing the guard; all upstream import
    # and conversion code executes with the guard active.
    loop = asyncio.new_event_loop()
    blocked: list[str] = []

    def offline_audit(event: str, _args: tuple[object, ...]) -> None:
        if event in {
            "socket.connect", "socket.connect_ex", "socket.getaddrinfo",
            "socket.gethostbyname", "socket.gethostbyaddr", "socket.getnameinfo",
            "socket.sendto",
            "subprocess.Popen", "os.system", "os.posix_spawn", "os.spawn",
            "os.exec", "os.fork", "os.forkpty",
        }:
            blocked.append(event)
            raise RuntimeError("Offline upstream probe blocked " + event)

    sys.addaudithook(offline_audit)
    from pydantic import ValidationError
    from aistudio_api.api.openai_errors import OpenAIInputError
    from aistudio_api.api.openai_schemas import OpenAIChatRequest
    from aistudio_api.application.openai_request import to_gemini_request

    raw = sys.stdin.buffer.read(8 * 1024 * 1024 + 1)
    if len(raw) > 8 * 1024 * 1024:
        raise ValueError("stdin exceeds the 8 MiB fixture limit")
    inputs = json.loads(raw)
    if not isinstance(inputs, list):
        raise ValueError("stdin must contain a JSON array")
    if len(inputs) > 128:
        raise ValueError("stdin exceeds the 128 fixture limit")

    async def convert() -> list[dict[str, object]]:
        outputs: list[dict[str, object]] = []
        for value in inputs:
            try:
                request = OpenAIChatRequest.model_validate(value)
                converted = await to_gemini_request(request)
                outputs.append({"accepted": True, "request": converted.model_dump(
                    mode="json", exclude_none=True
                )})
            except OpenAIInputError as exc:
                outputs.append({"accepted": False, "status": exc.status_code})
            except ValidationError:
                outputs.append({"accepted": False, "status": 400})
        return outputs

    try:
        outputs = loop.run_until_complete(convert())
        if blocked:
            raise RuntimeError("Offline audit violations: " + ", ".join(blocked))
        json.dump(outputs, sys.stdout, ensure_ascii=False, allow_nan=False)
        sys.stdout.write("\n")
    finally:
        loop.close()


if __name__ == "__main__":
    main()
