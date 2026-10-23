# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Measure native Gemini SSE arrival times without printing credentials or text.

Uses one real generation on the configured loopback service. It does not start,
stop or reconfigure the service. All output is timing, counts and finish reasons.
"""

import argparse
import http.client
import http.cookiejar
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--config', type=Path, required=True)
    args.add_argument('--model', required=True)
    args.add_argument('--thinking-level', choices=['MINIMAL', 'LOW', 'MEDIUM', 'HIGH'])
    args.add_argument('--sillytavern', help='Optional loopback SillyTavern origin, e.g. http://127.0.0.1:8000')
    options = args.parse_args()
    config = json.loads(options.config.read_text(encoding='utf-8-sig'))
    if config.get('bind') not in ('127.0.0.1', 'localhost', '::1'):
        raise ValueError('loopback_service_required')
    if not config.get('api_key'):
        raise ValueError('configured_api_key_required')
    thinking = {'includeThoughts': True}
    if options.thinking_level:
        thinking['thinkingLevel'] = options.thinking_level
    else:
        thinking['thinkingBudget'] = 0
    body = {'contents': [{'role': 'user', 'parts': [{'text':
        '请写一篇约八百字的中文童话，讲述一只狐狸在森林里寻找星星的故事。直接写故事。'}]}],
        'generationConfig': {'maxOutputTokens': 2048, 'thinkingConfig': thinking}}
    path = '/v1beta/models/' + urllib.parse.quote(options.model, safe='') + ':streamGenerateContent?alt=sse'
    connection = http.client.HTTPConnection(config['bind'], config['port'], timeout=180)
    began = time.monotonic()
    ms = lambda: round((time.monotonic() - began) * 1000, 1)
    try:
        if options.sillytavern:
            origin = urllib.parse.urlsplit(options.sillytavern)
            if (origin.scheme != 'http' or origin.hostname not in ('127.0.0.1', 'localhost', '::1')
                    or origin.username or origin.password or origin.path not in ('', '/') or origin.query or origin.fragment):
                raise ValueError('loopback_sillytavern_origin_required')
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
            base = options.sillytavern.rstrip('/')
            with opener.open(base + '/csrf-token', timeout=10) as csrf:
                csrf_token = json.load(csrf)['token']
            st_body = {'chat_completion_source': 'makersuite', 'model': options.model,
                'messages': [{'role': 'user', 'content': body['contents'][0]['parts'][0]['text']}],
                'stream': True, 'max_tokens': 2048, 'include_reasoning': True,
                'reverse_proxy': f"http://127.0.0.1:{config['port']}", 'proxy_password': config['api_key']}
            began = time.monotonic()
            response = opener.open(urllib.request.Request(base + '/api/backends/chat-completions/generate',
                json.dumps(st_body).encode(), {'Content-Type': 'application/json', 'X-CSRF-Token': csrf_token}), timeout=180)
        else:
            connection.request('POST', path, json.dumps(body).encode(), {
                'Content-Type': 'application/json', 'Authorization': 'Bearer ' + config['api_key']})
            response = connection.getresponse()
        print(json.dumps({'status': response.status, 'headers_ms': ms(),
                          'content_type': response.getheader('Content-Type')}), flush=True)
        if response.status != 200:
            return 1
        data = []
        count = 0
        while line := response.readline():
            line = line.rstrip(b'\r\n')
            if line.startswith(b'data:'):
                data.append(line[5:].lstrip(b' '))
            elif not line and data:
                event = json.loads(b'\n'.join(data))
                data.clear()
                count += 1
                record = {'event': count, 'ms': ms(), 'text_chars': 0, 'thought_chars': 0}
                for candidate in event.get('candidates', []):
                    for part in candidate.get('content', {}).get('parts', []):
                        text = part.get('text', '')
                        record['thought_chars' if part.get('thought') else 'text_chars'] += len(text)
                    if 'finishReason' in candidate:
                        record['finish'] = candidate['finishReason']
                if 'error' in event:
                    record['error'] = True
                print(json.dumps(record), flush=True)
        print(json.dumps({'done_ms': ms(), 'events': count, 'truncated_record': bool(data)}), flush=True)
        return 0 if count and not data else 1
    finally:
        connection.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, http.client.HTTPException) as error:
        # Exception messages can contain a URL or server response. Keep only type.
        print(json.dumps({'failure': type(error).__name__}), flush=True)
        raise SystemExit(1)
