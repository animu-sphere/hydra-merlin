"""Require executed Vulkan native, generated and usdview Forward capability tests."""

import argparse
from pathlib import Path
import xml.etree.ElementTree as ET


HOST = {
    'merlin-install-consumer',
    'merlin-vulkan-install-runtime',
    'merlin-vulkan-material-resources',
    'merlin-hydra2-hgi-vulkan-aov',
    'merlin-hydra2-forward-lighting-native-comparison',
}
for kind in ('lighting', 'motion', 'navigation'):
    for mode in ('tier0', 'hgi-vulkan', 'comparison'):
        HOST.add(f'merlin-hydra2-forward-{kind}-{mode}')

NATIVE = {
    'merlin-output-transform',
    'merlin-vulkan-material-resources',
    'merlin-vulkan-install-runtime',
}
for mode in ('conventional', 'bindless', 'gpu-scene', 'gpu-driven'):
    NATIVE.add(f'merlin-vulkan-output-{mode}')

GENERATED = {
    'merlin-vulkan-generated-material',
    'merlin-vulkan-material-resources',
}

SUITES = {'host': HOST, 'native': NATIVE, 'generated': GENERATED}


def verify(path, required):
    cases = {}
    for case in ET.parse(path).getroot().iter('testcase'):
        name = case.get('name')
        if name in cases:
            raise ValueError(f'duplicate test: {name}')
        cases[name] = case
    missing = required - cases.keys()
    if missing:
        raise ValueError(f'missing Forward runtime tests: {sorted(missing)}')
    for name in sorted(required):
        case = cases[name]
        if case.get('status') not in (None, 'run') or any(
            case.find(tag) is not None for tag in ('skipped', 'failure', 'error')
        ):
            raise ValueError(f'Forward runtime test did not pass: {name}')
    print(f'All {len(required)} required Forward runtime tests passed without skips')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--junit', type=Path, required=True)
    parser.add_argument('--suite', choices=sorted(SUITES), default='host')
    args = parser.parse_args()
    verify(args.junit, SUITES[args.suite])


if __name__ == '__main__':
    main()
