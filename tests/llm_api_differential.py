#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Compare real converters using independently authored, offline requests.

No account/configuration files or network endpoints are used. These comparisons
cover request normalization, not upstream wire acceptance or model behavior.
"""
import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path
import subprocess


def corpus():
    base = {'model': 'gemini-3.1-flash-lite', 'messages': [{'role': 'user', 'content': 'hello'}]}
    cases = []
    def add(name, **changes):
        request = copy.deepcopy(base)
        request.update(changes)
        cases.append({'name': name, 'input': request})
    function = {'name': 'lookup', 'description': 'Look up a value.',
                'parameters': {'type': 'object', 'properties': {'tag': {'type': 'string'}}, 'required': ['tag']}}
    tools = [{'type': 'function', 'function': function}]
    schema = {'type': 'object', 'properties': {'value': {'type': 'integer'}}, 'required': ['value']}
    add('text')
    add('leading_system', messages=[{'role': 'system', 'content': 'Use short answers.'}, *base['messages']])
    add('late_system', messages=[*base['messages'], {'role': 'assistant', 'content': 'Hi.'},
                               {'role': 'system', 'content': 'Use short answers.'}, {'role': 'user', 'content': 'Continue.'}])
    add('message_name', messages=[{'role': 'user', 'name': 'Alex', 'content': 'hello'}])
    add('assistant_refusal', messages=[*base['messages'], {'role': 'assistant', 'content': None, 'refusal': 'No.'},
                                     {'role': 'user', 'content': 'Continue.'}])
    add('sampling_stop', temperature=0.2, top_p=0.8, max_tokens=100, stop=['END'])
    add('numeric_string', temperature='0.2')
    add('out_of_range_temperature', temperature=4)
    add('dual_token_limits', max_tokens=80, max_completion_tokens=90)
    add('multiple_candidates', n=4)
    add('unknown_seed', seed=123)
    add('json_object', response_format={'type': 'json_object'})
    add('json_schema', response_format={'type': 'json_schema', 'json_schema': {'name': 'sample', 'schema': schema}})
    add('strict_schema', response_format={'type': 'json_schema', 'json_schema': {'name': 'sample', 'schema': schema, 'strict': True}})
    add('functions_auto', tools=tools, tool_choice='auto')
    add('legacy_functions', functions=[function], function_call='auto')
    add('required_function', tools=tools, tool_choice='required')
    add('named_function', tools=tools, tool_choice={'type': 'function', 'function': {'name': 'lookup'}})
    add('reasoning_alias', reasoning={'effort': 'low'})
    add('reasoning_xhigh', reasoning_effort='xhigh')
    add('native_generation_root', generationConfig={'temperature': 0.3})
    add('ais2api_generation', ais2api={'gemini': {'generationConfig': {'temperature': 0.3}}})
    add('thinking_history', messages=[*base['messages'], {'role': 'assistant', 'content': 'Answer.', 'reasoning_content': 'Consider.'},
                                    {'role': 'user', 'content': 'Continue.'}])
    add('input_text_alias', messages=[{'role': 'user', 'content': [{'type': 'input_text', 'text': 'hello'}]}])
    pdf = 'data:application/pdf;base64,' + base64.b64encode(b'%PDF-1.4\n%%EOF\n').decode()
    add('inline_file', messages=[{'role': 'user', 'content': [{'type': 'file', 'file': {'file_data': pdf}}]}])
    add('invalid_image_base64', messages=[{'role': 'user', 'content': [{'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,%%%%'}}]}])
    add('wrong_image_magic', messages=[{'role': 'user', 'content': [{'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,YWJj'}}]}])
    calls = [{'id': 'call_a', 'type': 'function', 'function': {'name': 'lookup', 'arguments': '{"tag":"A"}'}},
             {'id': 'call_b', 'type': 'function', 'function': {'name': 'lookup', 'arguments': '{"tag":"B"}'}}]
    history = [*base['messages'], {'role': 'assistant', 'content': None, 'tool_calls': calls}]
    add('unanswered_tool_batch', messages=history, tools=tools)
    results = [{'role': 'tool', 'tool_call_id': c['id'], 'content': '{"value":1}'} for c in calls]
    add('complete_tool_batch', messages=history + results, tools=tools)
    add('duplicate_tool_result', messages=history + results + [results[0]], tools=tools)
    add('empty_messages', messages=[])
    return cases


def canonical(value):
    if isinstance(value, dict):
        out = {k: canonical(v) for k, v in value.items() if v is not None}
        # Upstream always serializes an empty generationConfig. Neither probe
        # has model defaults; its absence and {} request the same default config.
        if out.get('generationConfig') == {}:
            del out['generationConfig']
        return out
    return [canonical(v) for v in value] if isinstance(value, list) else value


def diff_paths(a, b, path=''):
    if type(a) != type(b):
        return [path or '/']
    if isinstance(a, dict):
        return [p for k in sorted(a.keys() | b.keys()) for p in
                ([path + '/' + k] if k not in a or k not in b else diff_paths(a[k], b[k], path + '/' + k))]
    if isinstance(a, list):
        if len(a) != len(b):
            return [path + '/length']
        return [p for i, (x, y) in enumerate(zip(a, b)) for p in diff_paths(x, y, path + '/' + str(i))]
    # Numeric representation 0 vs 0.0 is harmless but type checks above retain
    # this conservative distinction; equality is not a whole-API parity claim.
    return [] if a == b else [path or '/']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpp-probe', type=Path, required=True)
    parser.add_argument('--upstream-python', type=Path, required=True)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    upstream_commit = '3e2eff888b21cc403168ceaaa1379a527d8b1a1c'
    checkout = args.upstream.resolve()
    git = ['git', '-c', 'safe.directory=' + checkout.as_posix(), '-C', str(checkout)]
    revision = subprocess.run(git + ['rev-parse', 'HEAD'], capture_output=True, text=True, timeout=15)
    clean = subprocess.run(git + ['diff', '--quiet', 'HEAD', '--', 'src/aistudio_api'], capture_output=True, timeout=15)
    extra = subprocess.run(git + ['ls-files', '--others', '--exclude-standard', '--', 'src/aistudio_api'], capture_output=True, text=True, timeout=15)
    if revision.returncode or revision.stdout.strip() != upstream_commit or clean.returncode or extra.returncode or extra.stdout.strip():
        raise RuntimeError('Upstream checkout must match the unmodified pinned commit')
    cases = corpus()
    payload = json.dumps([c['input'] for c in cases], allow_nan=False)
    probe = Path(__file__).with_name('llm_upstream_probe.py').resolve()
    commands = [[str(args.cpp_probe.resolve())], [str(args.upstream_python.resolve()), str(probe), '--source', str(args.upstream.resolve())]]
    outputs = []
    for command in commands:
        process = subprocess.run(command, input=payload, capture_output=True, text=True, timeout=90)
        if process.returncode:
            raise RuntimeError('Offline normalization probe failed; inspect its private development logs')
        values = json.loads(process.stdout)
        if len(values) != len(cases):
            raise RuntimeError('Probe returned an incomplete corpus')
        outputs.append(values)
    report = {'upstream_commit': upstream_commit,
              'cpp_probe_sha256': hashlib.sha256(args.cpp_probe.read_bytes()).hexdigest(),
              'scope': 'offline_synthetic_openai_request_normalization', 'generation_requests': 0, 'cases': []}
    for fixture, cpp, original in zip(cases, *outputs):
        differences = diff_paths(canonical(cpp), canonical(original))
        report['cases'].append({**fixture, 'cpp': cpp, 'original': original,
                                'normalized_equal': not differences, 'differing_paths': differences})
    report['case_count'] = len(cases)
    report['equal_count'] = sum(x['normalized_equal'] for x in report['cases'])
    report['different_count'] = len(cases) - report['equal_count']
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ['scope', 'generation_requests', 'case_count', 'equal_count', 'different_count']}))
    for case in report['cases']:
        print(json.dumps({'name': case['name'], 'cpp_accepted': case['cpp']['accepted'],
                          'original_accepted': case['original']['accepted'], 'equal': case['normalized_equal']}))


if __name__ == '__main__':
    main()
