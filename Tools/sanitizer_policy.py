#!/usr/bin/env python3
"""Explicit host sanitizer policy; never treats unsupported leak checking as a pass."""
import os
import sys


def sanitizer_environment(base=None, host=None):
    """Return an isolated environment and the exact enabled instrumentation scope.

    Apple's Xcode ASan runtime aborts when detect_leaks=1 is requested. Keep
    address and undefined-behavior checking fatal on every supported test host,
    but enable the leak runtime only on Linux. Never retry a failed test with
    instrumentation disabled, and never mutate the caller's environment.
    """
    host = sys.platform if host is None else host
    env = dict(os.environ if base is None else base)
    leaks = host.startswith('linux')
    env['ASAN_OPTIONS'] = 'detect_leaks={}:halt_on_error=1'.format(int(leaks))
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    scope = {
        'host_platform': host,
        'address_sanitizer_enabled': True,
        'undefined_behavior_sanitizer_enabled': True,
        'leak_detection_enabled': leaks,
        'leak_detection_scope': 'enabled on Linux' if leaks else 'not requested on this host; not verified',
        'asan_options': env['ASAN_OPTIONS'],
        'ubsan_options': env['UBSAN_OPTIONS'],
    }
    return env, scope
