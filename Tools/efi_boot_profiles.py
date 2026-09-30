"""Derive controlled boot profiles from a fully assembled OpenCore configuration."""

import copy


APPLE_BOOT_GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
MELLOW_BUNDLE = 'Mellow.kext'


def _disable_mellow(config):
    """Return a profile that changes only Mellow loading and its boot argument."""
    profile = copy.deepcopy(config)
    entries = [entry for entry in profile['Kernel']['Add']
               if entry['BundlePath'] == MELLOW_BUNDLE]
    if len(entries) != 1 or entries[0]['Enabled'] is not True:
        raise ValueError('Expected exactly one enabled Mellow.kext entry')
    arguments = profile['NVRAM']['Add'][APPLE_BOOT_GUID]['boot-args'].split()
    if arguments.count('-mellowdiag') != 1 or '-mellowoff' in arguments:
        raise ValueError('Expected one diagnostic boot argument and no disable argument')
    entries[0]['Enabled'] = False
    profile['NVRAM']['Add'][APPLE_BOOT_GUID]['boot-args'] = ' '.join(
        '-mellowoff' if argument == '-mellowdiag' else argument
        for argument in arguments)
    return profile


def build_profiles(config):
    """Return the baseline, rescue, and memory-map comparison profiles."""
    baseline = _disable_mellow(config)
    rescue = copy.deepcopy(baseline)
    rescue_args = rescue['NVRAM']['Add'][APPLE_BOOT_GUID]['boot-args']
    if 'cpus=1' in rescue_args.split():
        raise ValueError('Default boot arguments already limit CPU count')
    rescue['NVRAM']['Add'][APPLE_BOOT_GUID]['boot-args'] = rescue_args + ' cpus=1'
    legacy = copy.deepcopy(config)
    legacy['Booter']['Quirks'].update(
        DevirtualiseMmio=False, EnableWriteUnprotector=True,
        RebuildAppleMemoryMap=False, SyncRuntimePermissions=False)
    return {
        'config-boot-baseline.plist': baseline,
        'config-rescue.plist': rescue,
        'config-legacy-memory-map.plist': legacy,
    }
