"""Compare UnityEntry field offsets with a UnityDumper CppSDK; never modifies offsets."""
import argparse
import hashlib
import re
import sys
from pathlib import Path
from gen_offsets import FIELD_SPEC
from sdk_offsets import (DEFAULT_SDK, FIELD_SPEC as SDK_FIELD_SPEC,
                         build_artifacts, read_sdk, resolve_field)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_HEADER = ROOT / '2_src_port' / 'robrun_offsets.h'
DEFINE = re.compile(r'^#define\s+(RR_OFF_\w+)\s+(0x[0-9A-Fa-f]+)u?\b')


def read_defines(text, source):
    defines = {}
    for line in text.splitlines():
        match = DEFINE.match(line)
        if match:
            if match[1] in defines:
                raise ValueError(f'{source}: duplicate definition of {match[1]}')
            defines[match[1]] = int(match[2], 16)
    return defines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=DEFAULT_SDK, help='CppSDK directory or Models.hpp')
    parser.add_argument('--header', type=Path, default=DEFAULT_HEADER, help='UnityEntry robrun_offsets.h')
    parser.add_argument('--sdk-header', type=Path, help='Supplemental header (default: beside --header)')
    parser.add_argument('--binary', type=Path, help='Optional current GameAssembly.dll for build hash check')
    args = parser.parse_args()
    try:
        header = args.header.read_text(encoding='utf-8')
        defines = read_defines(header, args.header)
        sdk_header_path = args.sdk_header or args.header.with_name('robrun_sdk_offsets.h')
        sdk_header = sdk_header_path.read_text(encoding='utf-8')
        sdk_defines = read_defines(sdk_header, sdk_header_path)
        duplicates = set(defines) & set(sdk_defines)
        if duplicates:
            raise ValueError(f'Definitions overlap between headers: {sorted(duplicates)}')
        defines.update(sdk_defines)
        records, sources = read_sdk(args.sdk)
        generated_header, _ = build_artifacts(records, sources)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr); return 2
    specs = [(macro, None, cls, field) for macro, cls, field in FIELD_SPEC] + SDK_FIELD_SPEC
    match = mismatch = missing = ambiguous = 0
    for label, assembly, cls, field in specs:
        expected = defines.get(label)
        if expected is None:
            missing += 1; print(f'MISSING {label}: {cls}.{field}'); continue
        try:
            got = resolve_field(records, cls, field, assembly).offset
        except ValueError as exc:
            if str(exc).startswith('AMBIGUOUS'): ambiguous += 1
            else: missing += 1
            print(f'{label}: {exc}'); continue
        if got != expected:
            mismatch += 1; print(f'CHANGED {label}: {cls}.{field}: code=0x{expected:X}, sdk=0x{got:X}'); continue
        match += 1
    print(f'Fields: {match} match, {mismatch} changed, {missing} missing, {ambiguous} ambiguous')
    if sdk_header != generated_header:
        print('SDK header: CHANGED (definitions or source fingerprints differ; regenerate with tools/sdk_offsets.py)')
        mismatch += 1
    else:
        print('SDK header: matches generated definitions and verified source fingerprints')
    print('This checks field layouts only; it does not validate runtime pointers, methods, or generic layouts.')
    if args.binary:
        match_hash = re.search(r'GameAssembly SHA-256:\s*([0-9a-fA-F]{64})', header)
        try:
            sha = hashlib.sha256()
            with args.binary.open('rb') as binary:
                for chunk in iter(lambda: binary.read(1024*1024), b''): sha.update(chunk)
        except OSError as exc: print(f'ERROR: {exc}', file=sys.stderr); return 2
        if not match_hash: print('Binary hash: no baseline in offset header'); missing += 1
        elif sha.hexdigest().lower() != match_hash[1].lower():
            print(f'Binary hash: CHANGED (expected {match_hash[1].lower()}, found {sha.hexdigest()})')
            mismatch += 1
        else: print('Binary hash: matches the offsets header baseline')
    return 1 if mismatch or missing or ambiguous else 0

if __name__ == '__main__':
    raise SystemExit(main())

