"""Require executed Vulkan native and usdview Forward capability tests."""

import argparse
from pathlib import Path
import xml.etree.ElementTree as ET


REQUIRED = {
    'merlin-install-consumer',
    'merlin-vulkan-install-runtime',
    'merlin-vulkan-material-resources',
    'merlin-hydra2-hgi-vulkan-aov',
    'merlin-hydra2-forward-lighting-native-comparison',
}
for kind in ('lighting', 'motion', 'navigation'):
    for mode in ('tier0', 'hgi-vulkan', 'comparison'):
        REQUIRED.add(f'merlin-hydra2-forward-{kind}-{mode}')


def verify(path):
    cases = {}
    for case in ET.parse(path).getroot().iter('testcase'):
        name = case.get('name')
        if name in cases:
            raise ValueError(f'duplicate test: {name}')
        cases[name] = case
    missing = REQUIRED - cases.keys()
    if missing:
        raise ValueError(f'missing Forward runtime tests: {sorted(missing)}')
    for name in sorted(REQUIRED):
        case = cases[name]
        if case.get('status') not in (None, 'run') or any(
            case.find(tag) is not None for tag in ('skipped', 'failure', 'error')
        ):
            raise ValueError(f'Forward runtime test did not pass: {name}')
    print(f'All {len(REQUIRED)} required Forward runtime tests passed without skips')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--junit', type=Path, required=True)
    args = parser.parse_args()
    verify(args.junit)


if __name__ == '__main__':
    main()
