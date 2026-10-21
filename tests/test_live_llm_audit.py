#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Offline checks for the opt-in audit's cost and service-state safeguards."""
import tempfile
import unittest
from unittest.mock import patch

from live_llm_audit import Audit, AuditError, cheap


class Safeguards(unittest.TestCase):
    def audit(self):
        audit = Audit.__new__(Audit)
        audit.config = {'model_aliases': {}}
        audit.calls = 0
        audit.report = {}
        audit.opened = []
        audit.open = lambda path, body: audit.opened.append((path, body))
        return audit

    def test_only_cheap_model_families(self):
        for model in ['gemini-3.1-flash-lite', 'gemini-2.5-flash-lite', 'gemma-3-4b-it']:
            self.assertTrue(cheap(model))
        for model in ['gemini-3-pro', 'gemini-2.5-flash', 'models/gemini-3.1-flash-lite', '', 'gemma-3\n']:
            self.assertFalse(cheap(model))

    def test_reject_expensive_and_redirected_models_before_open(self):
        audit = self.audit()
        with self.assertRaises(AuditError):
            audit.generation('/v1/chat/completions', {'model': 'gemini-3-pro'})
        audit.config['model_aliases'] = {'gemini-3.1-flash-lite': 'gemini-3-pro'}
        with self.assertRaises(AuditError):
            audit.generation('/v1/chat/completions', {'model': 'gemini-3.1-flash-lite'})
        self.assertEqual(audit.calls, 0)
        self.assertEqual(audit.opened, [])

    def test_native_and_openai_share_five_request_budget(self):
        audit = self.audit()
        audit.generation('/v1beta/models/gemini-3.1-flash-lite:streamGenerateContent?alt=sse', {})
        for _ in range(4):
            audit.generation('/v1/chat/completions', {'model': 'gemma-3-4b-it'})
        with self.assertRaises(AuditError):
            audit.generation('/v1/chat/completions', {'model': 'gemini-3.1-flash-lite'})
        self.assertEqual(audit.calls, 5)
        self.assertEqual(len(audit.opened), 5)

    def test_effective_aliases_control_cost_guard(self):
        audit = self.audit()
        audit.config.update(bind='127.0.0.1', port=18451)
        audit.report = {}
        with tempfile.TemporaryDirectory() as data:
            effective = {**audit.config, 'data_dir': data, 'limits': {'max_attempts': 1},
                         'model_aliases': {'gemini-3.1-flash-lite': 'gemini-3-pro'}}
            def cli(*args):
                return {'config': effective} if args == ('config', 'show') else {'running': False}
            audit.cli = cli
            audit.setup()
            with self.assertRaises(AuditError):
                audit.generation('/v1/chat/completions', {'model': 'gemini-3.1-flash-lite'})
        self.assertEqual(audit.calls, 0)

    def test_lost_stop_reply_requires_confirmed_offline_status(self):
        for still_running in [False, True]:
            audit = self.audit()
            audit.report = {}; audit.managed = True; audit.was_running = False
            audit.stop_and_wait = lambda: (_ for _ in ()).throw(AuditError('lost_reply'))
            audit.cli = lambda *args: {'running': still_running}
            if still_running:
                with self.assertRaises(AuditError):
                    audit.restore()
                self.assertNotIn('prior_running_state_restored', audit.report)
            else:
                audit.restore()
                self.assertTrue(audit.report['prior_running_state_restored'])

    def test_stop_reply_loss_during_setup_is_resolved_by_status(self):
        for after_stop in [False, True]:
            audit = self.audit()
            statuses = iter([True, after_stop])
            def cli(*args):
                if args == ('stop',):
                    raise AuditError('lost_reply')
                return {'running': next(statuses)}
            audit.cli = cli
            if after_stop:
                with patch('live_llm_audit.time.monotonic', side_effect=[0, 100]):
                    with self.assertRaises(AuditError):
                        audit.stop_and_wait()
            else:
                audit.stop_and_wait()

    def test_draining_control_pipe_is_not_treated_as_stopped(self):
        audit = self.audit()
        statuses = iter([True, 'draining', False])
        def cli(*args):
            if args == ('stop',):
                return {'status': 'stopping'}
            value = next(statuses)
            if value == 'draining':
                audit.report['last_control_failure'] = {'reason': 'control_io'}
                raise AuditError('local_control_failed')
            return {'running': value}
        audit.cli = cli
        with patch('live_llm_audit.time.sleep'):
            audit.stop_and_wait()


if __name__ == '__main__':
    unittest.main()
