#!/usr/bin/env python3
"""Check ssf.games.txt against real CS:S engine_srv.so binaries (linux x86 / x64).

Usage:
    tests/verify_gamedata.py <css-ds-root>
    tests/verify_gamedata.py --engine32 bin/engine_srv.so --engine64 bin/linux64/engine_srv.so

For every linux "@symbol" signature, checks that the symbol exists in the engine
symbol table. For CFrameSnapshotManager__m_pPackedData, disassembles
CFrameSnapshotManager::LevelChanged and checks that the offset it clears
(MAX_EDICTS pointers) is the one declared in the gamedata.

Needs binutils (nm, objdump). Re-run it after every CS:S engine update.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAMEDATA = os.path.join(HERE, '..', 'package', 'addons', 'sourcemod', 'gamedata', 'ssf.games.txt')
MAX_EDICTS = 2048
LEVELCHANGED = '_ZN21CFrameSnapshotManager12LevelChangedEv'


def parse_gamedata(path):
    """Minimal KeyValues reader: returns {section: {name: {key: value}}} for Signatures/Offsets."""
    text = open(path, encoding='utf-8').read()
    text = re.sub(r'//[^\n]*', '', text)
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack, root, key = [], {}, None
    cur = root
    for quoted, brace in tokens:
        if brace == '{':
            new = {}
            cur[key] = new
            stack.append(cur)
            cur, key = new, None
        elif brace == '}':
            cur = stack.pop()
        elif key is None:
            key = quoted
        else:
            cur[key] = quoted
            key = None
    out = {'Signatures': {}, 'Offsets': {}}
    for game in root.get('Games', {}).values():
        for section in out:
            out[section].update(game.get(section, {}))
    return out


def symbols(engine):
    res = subprocess.run(['nm', engine], capture_output=True, text=True, check=True)
    syms = {}
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def levelchanged_offset(engine, addr):
    """Return (offset, bytes cleared) of the array memset by LevelChanged."""
    res = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '-M', 'intel',
                          '--start-address=%#x' % addr, '--stop-address=%#x' % (addr + 0x40), engine],
                         capture_output=True, text=True, check=True)
    offset = size = None
    for line in res.stdout.splitlines():
        m = re.search(r'\badd\s+(?:e|r)(?:dx|di|ax|cx),0x([0-9a-f]+)$', line)
        if m and offset is None:
            offset = int(m.group(1), 16)
        m = re.search(r'\bmov\s+(?:e|r)(?:cx|dx),0x([0-9a-f]+)$', line)
        if m and size is None:
            size = int(m.group(1), 16)
        if re.search(r'\b(ret|jmp)\b', line):
            break
    return offset, size


def check(engine, platform, gamedata):
    ok = True
    syms = symbols(engine)
    print('== %s (%s)' % (engine, platform))
    for name, sig in sorted(gamedata['Signatures'].items()):
        value = sig.get(platform)
        if not value or not value.startswith('@'):
            continue
        found = value[1:] in syms
        ok &= found
        print('  [%s] %-45s %s' % ('OK' if found else 'FAIL', name, value))

    declared = gamedata['Offsets'].get('CFrameSnapshotManager__m_pPackedData', {}).get(platform)
    if declared is not None:
        if LEVELCHANGED not in syms:
            print('  [FAIL] %s not found, cannot check m_pPackedData' % LEVELCHANGED)
            return False
        offset, size = levelchanged_offset(engine, syms[LEVELCHANGED])
        ptr = 8 if platform == 'linux64' else 4
        # x86 clears with "rep stos dword" (count in ecx), x64 calls memset (bytes in edx)
        good = offset == int(declared) and size in (MAX_EDICTS, MAX_EDICTS * ptr)
        ok &= good
        print('  [%s] %-45s declared %s, LevelChanged clears %s (count %s)'
              % ('OK' if good else 'FAIL', 'CFrameSnapshotManager__m_pPackedData', declared, offset, size))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root', nargs='?', help='CS:S dedicated server root (contains bin/)')
    ap.add_argument('--engine32')
    ap.add_argument('--engine64')
    args = ap.parse_args()
    e32 = args.engine32 or (args.root and os.path.join(args.root, 'bin', 'engine_srv.so'))
    e64 = args.engine64 or (args.root and os.path.join(args.root, 'bin', 'linux64', 'engine_srv.so'))
    if not e32 and not e64:
        ap.error('give a server root or --engine32/--engine64')

    gamedata = parse_gamedata(GAMEDATA)
    ok = True
    for engine, platform in ((e32, 'linux'), (e64, 'linux64')):
        if engine and os.path.exists(engine):
            ok &= check(engine, platform, gamedata)
        elif engine:
            print('== %s missing, skipped' % engine)
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
