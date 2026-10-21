#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Opt-in, five-generation audit using only discovered Flash-Lite/Gemma models.

No credentials, model messages, media, tool IDs or signatures enter the report.
The service is temporarily started with one attempt; its prior running state is
restored. No configuration or account records are edited. Python is a test tool.
"""
from __future__ import annotations
import argparse
import base64
import copy
import datetime
import hashlib
import io
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import wave
import zlib


class AuditError(Exception):
    pass


def check(condition, label):
    if not condition:
        raise AuditError(label)


def cheap(model):
    return bool(re.fullmatch(r"(?:gemini-[a-z0-9.-]*flash-lite[a-z0-9.-]*|gemma-[a-z0-9.-]+)", model))


def red_png():
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))
    pixels = b''.join(b'\0' + b'\xff\0\0' * 8 for _ in range(8))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 8, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')


def tone_wav():
    out = io.BytesIO()
    with wave.open(out, 'wb') as wav:
        wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(16000)
        wav.writeframes(b''.join(struct.pack('<h', round(7000 * math.sin(2 * math.pi * 600 * n / 16000))) for n in range(6400)))
    return out.getvalue()


class Audit:
    def __init__(self, args):
        self.args = args
        self.config = json.loads(args.config.read_text(encoding='utf-8-sig'))
        self.key = self.config.get('api_key', '')
        check(bool(self.key), 'configured_api_key_required')
        check(self.config.get('bind') in ['127.0.0.1', 'localhost', '::1'], 'loopback_service_required')
        self.origin = 'http://127.0.0.1:' + str(self.config['port'])
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.was_running = False; self.managed = False; self.calls = 0
        self.report = {'date': '2026-10-21', 'upstream_commit': '3e2eff888b21cc403168ceaaa1379a527d8b1a1c',
                       'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       'binary_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                       'generation_limit': 5, 'automatic_attempt_limit': 1, 'cases': []}

    def cli(self, *parts):
        result = subprocess.run([str(self.args.exe), '--config', str(self.args.config), *parts],
                                cwd=self.args.exe.parent, capture_output=True, text=True, timeout=30)
        if result.returncode:
            try:
                failure = json.loads(result.stderr).get('error', {})
                reason = failure.get('reason', '')
                self.report['last_control_failure'] = {'command': parts[-1],
                    'reason': reason if isinstance(reason, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', reason) else 'unknown'}
            except (ValueError, AttributeError):
                pass
        check(result.returncode == 0, 'local_control_failed')
        return json.loads(result.stdout)

    def setup(self):
        self.report['stage'] = 'local_status'
        state = self.cli('status')
        check(not state.get('active') and state.get('queued', 0) == 0, 'service_busy')
        self.was_running = bool(state.get('running'))
        self.managed = True
        if self.was_running:
            self.stop_and_wait()
        self.report['stage'] = 'start_single_attempt_service'
        self.cli('--max-attempts', '1', 'start')
        self.report['stage'] = 'effective_configuration'
        effective = self.cli('config', 'show')['config']
        check(effective['limits']['max_attempts'] == 1, 'one_attempt_override_failed')
        check(effective['bind'] in ['127.0.0.1', 'localhost', '::1'] and
              effective['bind'] == self.config['bind'] and effective['port'] == self.config['port'],
              'effective_listener_mismatch')
        self.config = effective
        self.log = Path(effective['data_dir']) / 'service.log'
        self.log_offset = self.log.stat().st_size if self.log.exists() else 0

    def restore(self):
        if not self.managed:
            return
        try:
            self.stop_and_wait()
        except Exception:
            # Shutdown can close the control channel before its reply arrives.
            # Only a fresh, successful offline status check can resolve that.
            check(not self.cli('status').get('running'), 'service_still_running')
        if self.was_running:
            self.cli('start')
        self.report['prior_running_state_restored'] = True

    def stop_and_wait(self):
        if not self.cli('status').get('running'):
            return
        deadline = time.monotonic() + 20
        try:
            self.cli('stop')
        except Exception:
            # The stop reply may be lost while the pipe is draining. Confirm
            # shutdown below; a lost observation never authorizes a new start.
            pass
        while True:
            try:
                if not self.cli('status').get('running'):
                    return
            except AuditError:
                check(self.report.get('last_control_failure', {}).get('reason') in
                      ['control_io', 'control_timeout', 'control_stopped'], 'service_stop_not_confirmed')
            check(time.monotonic() < deadline, 'service_stop_timeout')
            time.sleep(0.2)

    def open(self, path, body=None, key=None):
        headers = {'Authorization': 'Bearer ' + (self.key if key is None else key)}
        data = None
        if body is not None:
            headers['Content-Type'] = 'application/json'
            data = json.dumps(body, allow_nan=False).encode()
        request = urllib.request.Request(self.origin + path, data=data, headers=headers)
        return self.opener.open(request, timeout=110)

    def json_request(self, path, body=None):
        with self.open(path, body) as response:
            return self.read_json(response)

    def read_json(self, response):
        raw = response.read(1024 * 1024 + 1)
        check(len(raw) <= 1024 * 1024, 'response_limit')
        return json.loads(raw)

    def generation(self, path, body):
        model = body.get('model') or urllib.parse.unquote(path.split('/models/')[1].split(':')[0])
        check(cheap(model), 'disallowed_model')
        aliases = self.config.get('model_aliases', {})
        check(model not in aliases or aliases[model] == model, 'model_alias_not_allowed')
        check(self.calls < 5, 'generation_budget_exceeded')
        self.calls += 1
        return self.open(path, body)

    def case(self, name, fn):
        record = {'name': name}; start = time.monotonic()
        try:
            record.update(fn() or {}); record.setdefault('passed', True)
        except urllib.error.HTTPError as error:
            record.update(passed=False, http=error.code)
            try:
                reason = json.load(error).get('error', {}).get('code')
                record['reason'] = reason if isinstance(reason, (str, int)) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', str(reason)) else 'unspecified'
            except Exception:
                record['reason'] = 'unreadable_error'
        except AuditError as error:
            record.update(passed=False, reason=str(error))
        except Exception:
            record.update(passed=False, reason='test_execution_failed')
        record['seconds'] = round(time.monotonic() - start, 3)
        self.report['cases'].append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)

    def consume(self, response, native=False):
        text = ''; chunks = 0; ids = set(); created = set(); done = False; usage = False; finish = False; size = 0
        finish_reasons = set(); token_counts = {}
        for line in response:
            size += len(line); check(size <= 1024 * 1024, 'stream_limit')
            if not line.startswith(b'data:'):
                continue
            data = line[5:].strip()
            if data == b'[DONE]':
                check(not native and not done, 'unexpected_done'); done = True; continue
            check(not done, 'event_after_done')
            event = json.loads(data); check('error' not in event, 'stream_error')
            chunks += 1
            if native:
                if event.get('responseId'): ids.add(event['responseId'])
                usage = usage or bool(event.get('usageMetadata'))
                for candidate in event.get('candidates', []):
                    finish = finish or bool(candidate.get('finishReason'))
                    if candidate.get('finishReason'): finish_reasons.add(candidate['finishReason'])
                    text += ''.join(p.get('text', '') for p in candidate.get('content', {}).get('parts', []) if not p.get('thought'))
            else:
                if event.get('id'): ids.add(event['id'])
                if 'created' in event: created.add(event['created'])
                if event.get('usage'):
                    check(event.get('choices') == [], 'usage_chunk_choices'); usage = True
                    token_counts = {k: v for k, v in event['usage'].items() if k in
                                    ['prompt_tokens', 'completion_tokens', 'total_tokens', 'completion_tokens_details']}
                for candidate in event.get('choices', []):
                    finish = finish or bool(candidate.get('finish_reason'))
                    if candidate.get('finish_reason'): finish_reasons.add(candidate['finish_reason'])
                    text += candidate.get('delta', {}).get('content', '') or ''
        check(chunks > 0 and usage and finish and len(ids) == 1, 'incomplete_stream')
        check(native or (done and len(created) == 1), 'openai_stream_contract')
        return text.strip(), {'chunks': chunks, 'usage': usage, 'finish': finish, 'done': done,
                              'stable_id': True, 'stable_created': len(created) == 1 if not native else None,
                              'finish_reasons': sorted(finish_reasons), 'token_counts': token_counts,
                              'visible_characters': len(text)}

    def run(self):
        self.setup()
        self.report['stage'] = 'model_discovery'
        models = self.json_request('/v1/models')['data']
        available = [x['id'] for x in models if x.get('ais2api', {}).get('source') in ('live', 'cache')]
        flash = 'gemini-3.1-flash-lite'
        if flash not in available:
            flash = next((x for x in available if x.startswith('gemini-') and 'flash-lite' in x), '')
        check(flash and cheap(flash), 'no_discovered_flash_lite')
        gemmas = [x for x in available if x.startswith('gemma-') and cheap(x)]
        def size(model):
            match = re.search(r'(\d+(?:\.\d+)?)b', model)
            return (float(match[1]) if match else 999, model)
        gemma = min(gemmas, key=size) if gemmas else None
        self.report['models'] = {'flash_lite': flash, 'gemma': gemma, 'catalog_count': len(models)}
        self.report['stage'] = 'test_cases'

        def routes():
            native = self.json_request('/v1beta/models')['models']
            check(len(native) == len(models), 'catalog_count_mismatch')
            check(self.json_request('/v1/models/' + urllib.parse.quote(flash, safe=''))['id'] == flash, 'openai_model_detail')
            check(self.json_request('/v1beta/models/' + urllib.parse.quote(flash, safe=''))['name'] == 'models/' + flash, 'gemini_model_detail')
            try:
                self.open('/v1/models', key='audit-invalid-key')
                raise AuditError('invalid_key_accepted')
            except urllib.error.HTTPError as error:
                check(error.code == 401, 'invalid_key_status')
            return {'model_endpoints': 4, 'catalog_count': len(models), 'invalid_key_http': 401}
        self.case('routes_models_auth', routes)

        def combined():
            schema = {'type': 'object', 'properties': {'system': {'type': 'string'}, 'marker': {'type': 'string'},
                      'color': {'type': 'string', 'enum': ['red', 'green', 'blue']}}, 'required': ['system', 'marker', 'color']}
            body = {'model': flash, 'messages': [
                {'role': 'system', 'content': 'The system code is R7. Return only the requested JSON.'},
                {'role': 'user', 'content': 'Remember my marker B4.'},
                {'role': 'assistant', 'content': 'Remembered.'},
                {'role': 'user', 'content': [{'type': 'text', 'text': 'Return the system code, earlier marker, and dominant image color in system/marker/color fields.'},
                 {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,' + base64.b64encode(red_png()).decode()}}]}],
                'temperature': 0, 'top_p': 1, 'max_tokens': 128, 'tools': [], 'tool_choice': 'none',
                'response_format': {'type': 'json_schema', 'json_schema': {'name': 'audit', 'strict': False, 'schema': schema}}}
            with self.generation('/v1/chat/completions', body) as response:
                value = self.read_json(response)
            answer = json.loads(value['choices'][0]['message']['content'])
            checks = {'system': answer.get('system') == 'R7', 'history': answer.get('marker') == 'B4',
                      'image': answer.get('color') == 'red', 'json_schema': set(answer) == {'system', 'marker', 'color'},
                      'usage': bool(value.get('usage')), 'finish_stop': value['choices'][0]['finish_reason'] == 'stop'}
            check(all(checks.values()), 'combined_content_or_contract_mismatch')
            return {'model': flash, 'http': 200, 'checks': checks, 'parameter_scope': 'sampling_and_limit_accepted_not_statistically_proven'}
        self.case('flash_lite_system_history_schema_image', combined)

        def gemma_stream():
            check(gemma is not None, 'no_discovered_gemma')
            body = {'model': gemma, 'messages': [{'role': 'user', 'content': 'Reply with exactly: GEMMA'}],
                    'max_tokens': 512, 'temperature': 0, 'tools': [], 'tool_choice': 'none',
                    'stream': True, 'stream_options': {'include_usage': True}}
            with self.generation('/v1/chat/completions', body) as response:
                text, info = self.consume(response)
            return {'model': gemma, 'http': 200, 'content_matches': text == 'GEMMA',
                    'passed': text == 'GEMMA', **info}
        self.case('gemma_openai_sse', gemma_stream)

        tools = [{'type': 'function', 'function': {'name': 'audit_lookup', 'description': 'Obtain the secret audit value for the requested tag.',
                  'parameters': {'type': 'object', 'properties': {'tag': {'type': 'string'}}, 'required': ['tag']}}}]
        history = [{'role': 'user', 'content': 'Call audit_lookup exactly once with tag X. After its result, reply exactly TOOL_OK_<value>. Do not invent the value.'}]
        call_message = None
        def invoke():
            nonlocal call_message
            body = {'model': flash, 'messages': history, 'tools': tools, 'tool_choice': 'auto', 'max_tokens': 192, 'temperature': 0}
            with self.generation('/v1/chat/completions', body) as response:
                value = self.read_json(response)
            message = value['choices'][0]['message']; calls = message.get('tool_calls', [])
            check(len(calls) == 1, 'model_did_not_return_one_tool_call')
            check(calls[0]['function']['name'] == 'audit_lookup' and json.loads(calls[0]['function']['arguments']) == {'tag': 'X'}, 'tool_arguments_mismatch')
            check(value['choices'][0]['finish_reason'] == 'tool_calls', 'tool_finish_reason')
            call_message = {'role': 'assistant', 'content': message.get('content'), 'tool_calls': copy.deepcopy(calls)}
            parts = message.get('ais2api', {}).get('gemini_content', {}).get('parts', [])
            return {'model': flash, 'http': 200, 'tool_calls': 1, 'arguments_match': True,
                    'native_signature_present': any(p.get('thoughtSignature') and p.get('functionCall') for p in parts)}
        self.case('flash_lite_tool_call_auto', invoke)

        def result():
            check(call_message is not None, 'tool_call_prerequisite_failed')
            messages = history + [call_message, {'role': 'tool', 'tool_call_id': call_message['tool_calls'][0]['id'], 'content': '{"value":9}'}]
            body = {'model': flash, 'messages': messages, 'tools': tools, 'tool_choice': 'auto',
                    'temperature': 0, 'max_tokens': 128, 'stream': True, 'stream_options': {'include_usage': True}}
            with self.generation('/v1/chat/completions', body) as response:
                text, info = self.consume(response)
            check(text == 'TOOL_OK_9', 'tool_result_content_mismatch')
            return {'model': flash, 'http': 200, 'content_matches': True, 'standard_tool_history_only': True, **info}
        self.case('flash_lite_standard_tool_result_sse', result)

        def native_audio():
            body = {'contents': [{'role': 'user', 'parts': [
                {'text': 'Does this audio contain intelligible spoken words? For no speech answer exactly TONE|END|TRAIL, otherwise answer exactly SPEECH|END|TRAIL.'},
                {'inlineData': {'mimeType': 'audio/wav', 'data': base64.b64encode(tone_wav()).decode()}}]}],
                'generationConfig': {'temperature': 0, 'maxOutputTokens': 96, 'stopSequences': ['|END|']}, 'tools': []}
            path = '/v1beta/models/' + urllib.parse.quote(flash, safe='') + ':streamGenerateContent?alt=sse'
            with self.generation(path, body) as response:
                text, info = self.consume(response, native=True)
            check(text == 'TONE', 'audio_or_stop_content_mismatch')
            return {'model': flash, 'http': 200, 'tone_identified': True, 'stop_marker_and_suffix_absent': True, **info}
        self.case('flash_lite_gemini_sse_audio_stop', native_audio)

        self.report['client_generation_requests'] = self.calls
        traces = []
        if self.log.exists():
            with self.log.open('rb') as log:
                log.seek(self.log_offset)
                for line in log:
                    try:
                        entry = json.loads(line)
                        if entry.get('event') == 'generation':
                            traces.append({'attempts': entry.get('attempts'), 'error': entry.get('error')})
                    except (ValueError, UnicodeError):
                        pass
        self.report['backend_generation_attempts'] = sum(x['attempts'] or 0 for x in traces)
        self.report['attempt_trace_count'] = len(traces)
        check(len(traces) == self.calls and all(x['attempts'] == 1 for x in traces), 'attempt_accounting_mismatch')
        self.report['all_cases_passed'] = all(x['passed'] for x in self.report['cases'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--real', action='store_true', required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    audit = Audit(args)
    try:
        audit.run()
    except urllib.error.HTTPError as error:
        audit.report['fatal'] = 'http_preflight_failed'
        audit.report['http'] = error.code
        try:
            reason = audit.read_json(error).get('error', {}).get('code')
            audit.report['reason'] = reason if isinstance(reason, (str, int)) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', str(reason)) else 'unspecified'
        except Exception:
            audit.report['reason'] = 'unreadable_error'
    except AuditError as error:
        audit.report['fatal'] = str(error)
    except Exception as error:
        audit.report['fatal'] = 'audit_execution_failed'
        audit.report['exception_type'] = type(error).__name__
    finally:
        try:
            audit.restore()
        except Exception as error:
            audit.report['restore_failed'] = True
            audit.report['restore_exception_type'] = type(error).__name__
        audit.report['client_generation_requests'] = audit.calls
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(audit.report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'generations': audit.calls, 'all_cases_passed': audit.report.get('all_cases_passed', False),
                      'state_restored': audit.report.get('prior_running_state_restored', False)}))
    return 0 if audit.report.get('all_cases_passed') and not audit.report.get('restore_failed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
