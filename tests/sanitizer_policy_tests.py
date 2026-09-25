#!/usr/bin/env python3
"""Regression tests for host-specific sanitizer options; no GPU or kernel execution."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('sanitizer_policy', Path(__file__).resolve().parents[1] / 'Tools/sanitizer_policy.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class SanitizerPolicyTests(unittest.TestCase):
    def test_darwin_keeps_asan_ubsan_but_does_not_request_unsupported_leaks(self):
        env, scope = policy.sanitizer_environment({'PATH': '/test'}, 'darwin')
        self.assertEqual(env['ASAN_OPTIONS'], 'detect_leaks=0:halt_on_error=1')
        self.assertIn('halt_on_error=1', env['UBSAN_OPTIONS'])
        self.assertTrue(scope['address_sanitizer_enabled'])
        self.assertTrue(scope['undefined_behavior_sanitizer_enabled'])
        self.assertIs(scope['leak_detection_enabled'], False)
        self.assertIn('not verified', scope['leak_detection_scope'])

    def test_linux_preserves_leak_detection(self):
        env, scope = policy.sanitizer_environment({}, 'linux')
        self.assertEqual(env['ASAN_OPTIONS'], 'detect_leaks=1:halt_on_error=1')
        self.assertTrue(scope['leak_detection_enabled'])

    def test_never_mutates_caller_environment(self):
        original = {'PATH': '/test', 'ASAN_OPTIONS': 'halt_on_error=0', 'UNRELATED': 'preserve'}
        snapshot = original.copy()
        env, scope = policy.sanitizer_environment(original, 'darwin')
        self.assertEqual(original, snapshot)
        self.assertEqual(env['UNRELATED'], 'preserve')
        self.assertEqual(env['ASAN_OPTIONS'], scope['asan_options'])
        self.assertEqual(env['UBSAN_OPTIONS'], scope['ubsan_options'])

    def test_unknown_platform_does_not_invent_leak_support(self):
        for host in ('win32', 'freebsd14', 'unknown'):
            with self.subTest(host=host):
                env, scope = policy.sanitizer_environment({}, host)
                self.assertFalse(scope['leak_detection_enabled'])
                self.assertIn('halt_on_error=1', env['ASAN_OPTIONS'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
