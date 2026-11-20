#!/usr/bin/env python3
"""Foreground tests. Only random self-created virtual endpoints may be opened."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

import kit

ROOT = Path(__file__).resolve().parents[2]


def check(condition, message):
    """Acceptance gate that still runs under python -O (bare asserts are compiled out)."""
    if not condition:
        raise SystemExit('FAIL: ' + message)


def load_native(path):
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        rows.append(kit.event((int(parts[0]) - 48000) / 48000,
                              [int(x, 16) for x in parts[2:]], 'replay'))
    return rows


def run(argv, run_dir, label, cwd=None):
    result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    (run_dir / (label + '.txt')).write_text((result.stdout or '') + (result.stderr or ''))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, required=True, help='existing pinned distingNT_API checkout (read only)')
    a = p.parse_args()
    sdk = a.sdk.resolve()
    gates = []

    def gate(name, condition, message):
        check(condition, message)
        gates.append(name)

    sdk_head = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
    gate('sdk-pinned', sdk_head == '5a4910d1d4233180114d6aee5ddaa4b8aec577e8', 'SDK pin mismatch: ' + sdk_head)
    run_dir = ROOT / 'build' / ('mpe-kit-' + uuid.uuid4().hex[:12])
    run_dir.mkdir(parents=True)
    print('EVIDENCE=' + str(run_dir), flush=True)
    unit = run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/mpe-kit', '-p', 'test_kit.py', '-v'],
               run_dir, 'unit-tests', cwd=ROOT)
    sys.stdout.write(unit.stdout)
    sys.stderr.write(unit.stderr)
    gate('unit-suite', unit.returncode == 0, 'unit/fake-backend suite failed; see ' + str(run_dir / 'unit-tests.txt'))
    ran = re.search(r'^Ran (\d+) tests?', unit.stderr or '', re.MULTILINE)
    gate('unit-count-observed', ran is not None, 'could not read the observed unit-test count')
    unit_tests = int(ran.group(1))
    gate('unit-count-nonzero', unit_tests > 0, 'no unit tests ran')
    compile_cmd = ['clang++', '-std=gnu++11', '-Wall', '-Wextra', '-Werror', '-O2',
                   '-DMIDIBUFFER_NATIVE_TEST=1', '-D_DISTINGNT_SERIALISATION_INTERNAL=1',
                   '-I' + str(sdk / 'include'), '-Isrc', '-Itests', 'src/midibuffer.cpp',
                   'tests/host_double.cpp', 'tools/mpe-kit/phrase_fixture.cpp', '-o', str(run_dir / 'phrase_fixture')]
    built = run(compile_cmd, run_dir, 'native-compile', cwd=ROOT)
    gate('native-compile', built.returncode == 0, 'native fixture compile failed')
    fixture = run([str(run_dir / 'phrase_fixture'), str(run_dir / 'native-direct.log'),
                   str(run_dir / 'native-replay.log')], run_dir, 'native-fixture', cwd=ROOT)
    gate('native-fixture', fixture.returncode == 0, 'native production fixture failed')
    native = load_native(run_dir / 'native-replay.log')
    direct = load_native(run_dir / 'native-direct.log')
    expected = kit.log('expected', kit.phrase())
    kit.write_json(run_dir / 'expected.json', expected)
    for name, rows in [('native-direct', direct), ('native-replay', native)]:
        result = kit.compare(expected, kit.log(name, rows), tolerance=4/48000)
        kit.write_json(run_dir / (name + '-report.json'), result)
        gate(name + '-pass', result['result'] == 'PASS', name + ' comparator did not PASS')
        gate(name + '-count', len(rows) == len(expected['events']), name + ' event count mismatch')
    # No physical outputs: both endpoints below are created here with fresh UUIDs.
    import rtmidi
    suffix = uuid.uuid4().hex
    sink_name, source_name = 'MidiBuffer kit sink ' + suffix, 'MidiBuffer kit source ' + suffix
    sink = rtmidi.MidiIn(queue_size_limit=kit.MAX_EVENTS)
    source = rtmidi.MidiOut()
    received = []
    opened = []
    try:
        sink.open_virtual_port(sink_name)
        sink.ignore_types(sysex=False, timing=False, active_sense=False)
        def observe(message, user_data=None):
            raw, _ = message
            if len(received) >= kit.MAX_EVENTS:
                raise RuntimeError('virtual sink received-count bound exceeded')
            received.append(kit.event(time.monotonic(), raw, 'music'))
        sink.set_callback(observe)
        source.open_virtual_port(source_name)
        base = kit.MacBackend()
        # Exact inventory and UID must identify our own newly created endpoints.
        out = [r for r in base.inventory('output') if r['name'] == sink_name]
        inp = [r for r in base.inventory('input') if r['name'] == source_name]
        gate('private-endpoints-unique', len(out) == len(inp) == 1,
             'self-created virtual endpoints were not uniquely inventoried')
        out, inp = out[0], inp[0]
        class IsolatedBackend:
            def inventory(self, direction):
                return base.inventory(direction)
            def check(self, selected):
                check(selected in (out, inp), 'PHYSICAL ACCESS DENIED by endpoint allowlist')
                return base.check(selected)
            def open(self, direction, selected):
                check(selected == (out if direction == 'output' else inp),
                      'PHYSICAL ACCESS DENIED by endpoint allowlist')
                opened.append(selected)
                return base.open(direction, selected)
        backend = IsolatedBackend()
        sent = kit.send(backend, out['name'], out['uid'], kit.CONFIRM, run_dir / 'sent.jsonl')
        gate('virtual-send-complete', sent['complete'], 'virtual generator send was incomplete: ' + str(sent.get('error')))
        # Bounded foreground drain, no detached sender or listener thread created by kit.
        time.sleep(.05)
        gate('virtual-send-received-count', len(received) == len(native),
             f'virtual sink received {len(received)} of {len(native)} events')
        observed = kit.log('virtual-generator-received', received)
        kit.write_json(run_dir / 'generator-received.json', observed)
        generator_result = kit.compare(expected, observed)
        kit.write_json(run_dir / 'generator-report.json', generator_result)
        gate('virtual-send-payload', generator_result['result'] == 'PASS', 'virtual generator comparator did not PASS')
        # Inject actual production trace onto only our self-created virtual source.
        # This is stimulus injection, never input -> output echo.
        index = 0
        begin = time.monotonic() + .1
        def inject(duration):
            nonlocal index
            time.sleep(duration)
            while index < len(native) and time.monotonic() >= begin + native[index]['t']:
                source.send_message(native[index]['raw'])
                index += 1
        captured = kit.capture(backend, inp['name'], inp['uid'], run_dir / 'replay.jsonl', 5,
                               sleep=inject, active_seconds=2.8)
        replay_result = kit.compare(expected, captured)
        kit.write_json(run_dir / 'replay-report.json', replay_result)
        gate('virtual-replay-payload', replay_result['result'] == 'PASS', 'virtual replay comparator did not PASS')
        gate('virtual-replay-count', len(captured['events']) == len(native),
             f'virtual replay captured {len(captured["events"])} of {len(native)} events')
        gate('virtual-injection-exhausted', index == len(native), 'native replay trace was not fully injected')
        negatives = {}
        for name in ('channel', 'expression', 'missing', 'extra', 'order', 'timing', 'empty', 'truncated'):
            data = copy.deepcopy(captured); rows = data['events']
            if name in ('channel', 'expression'):
                raw = rows[8]['raw'][:]
                if name == 'channel': raw[0] = 0xE3
                else: raw[1] = 1
                rows[8] = kit.event(rows[8]['t'], raw, 'replay')
            elif name == 'missing': rows.pop(8)
            elif name == 'extra': rows.insert(8, copy.deepcopy(rows[8]))
            elif name == 'order':
                x, y = rows[8]['t'], rows[9]['t']
                rows[8], rows[9] = rows[9], rows[8]
                rows[8]['t'], rows[9]['t'] = x, y
            elif name == 'timing': rows[8]['t'] += .05
            elif name == 'empty': rows.clear()
            elif name == 'truncated': data['complete'] = False
            data['count'] = len(rows)
            path, report = run_dir / (name + '.json'), run_dir / (name + '-report.json')
            kit.write_json(path, data)
            command = [sys.executable, str(ROOT / 'tools/mpe-kit/kit.py'), 'compare', '--expected',
                       str(run_dir / 'expected.json'), '--actual', str(path), '--require-replay',
                       '--report', str(report)]
            result = run(command, run_dir, name + '-cli')
            gate('negative-' + name, result.returncode == 1,
                 f'comparator negative {name} returned {result.returncode}, expected 1')
            negatives[name] = dict(exit=result.returncode, report=str(report))
        # No-confirm rejected by CLI before MacBackend is constructed.
        denied = run([sys.executable, str(ROOT / 'tools/mpe-kit/kit.py'), 'send', '--output',
                      'NEVER OPEN', '--uid', '0', '--confirm', 'no', '--log', str(run_dir / 'denied.jsonl')],
                     run_dir, 'no-confirm-cli')
        gate('no-confirm-exit', denied.returncode == 2, f'no-confirm returned {denied.returncode}, expected 2')
        gate('no-confirm-no-journal', not (run_dir / 'denied.jsonl').exists(), 'no-confirm created a journal')
        gate('no-physical-handle', all(s in (out, inp) for s in opened),
             'a handle outside the self-created virtual endpoints was opened')
        summary = dict(result='PASS', evidence=str(run_dir), unitTests=unit_tests,
                       expectedCount=len(native), generatorReceivedCount=len(received),
                       replayCapturedCount=len(captured['events']),
                       productionOracle='src/midibuffer.cpp via existing tests/receiver_fixture.cpp rig',
                       compileCommand=compile_cmd, endpoints=[out, inp], opened=opened,
                       physicalOutputsOpened=0, physicalSends=0, negatives=negatives, noConfirmExit=2,
                       gates=gates, interpreter=dict(python=sys.version.split()[0], optimized=not __debug__),
                       actualDevice='NOT ATTEMPTED')
        kit.write_json(run_dir / 'summary.json', summary)
        print(json.dumps(summary, indent=2))
    finally:
        sink.cancel_callback()
        sink.close_port()
        source.close_port()
    return 0


if __name__ == '__main__':
    sys.exit(main())
