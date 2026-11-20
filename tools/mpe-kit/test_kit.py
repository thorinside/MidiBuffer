import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import kit


class Clock:
    def __init__(self):
        self.now = 0
        self.hook = None
    def __call__(self):
        return self.now
    def sleep(self, duration):
        self.now += max(duration, .001)
        if self.hook:
            self.hook()


class Handle:
    def __init__(self, backend):
        self.backend = backend
        self.sent = []
        self.closed = False
        self.callback = None
        self.fail_on = None
    def send_message(self, raw):
        if self.fail_on is not None and len(self.sent) == self.fail_on:
            self.fail_on = None
            raise RuntimeError('send fault')
        self.sent.append(raw)
    def close_port(self):
        self.closed = True
    def set_callback(self, callback):
        self.callback = callback
    def cancel_callback(self):
        self.callback = None


class Backend:
    def __init__(self, direction='output'):
        self.selected = dict(name='owned', uid=7, direction=direction)
        self.rows = [self.selected, dict(name='NEVER OPEN', uid=9, direction=direction)]
        self.opened = []
        self.handle = Handle(self)
    def inventory(self, direction):
        return [r for r in self.rows if r['direction'] == direction]
    def open(self, direction, selected):
        assert selected == self.selected
        assert direction == self.selected['direction']
        self.opened.append((direction, selected))
        return self.handle


class KitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'log.jsonl'
        self.clock = Clock()
    def tearDown(self):
        self.tmp.cleanup()
    def run_send(self, backend, confirm=kit.CONFIRM):
        return kit.send(backend, 'owned', 7, confirm, self.path,
                        sleep=self.clock.sleep, clock=self.clock)
    def test_sender_deadline_crossing_between_clock_reads_never_sleeps_negative(self):
        # The ordinary fake Clock advances only in sleep and misses this race.
        class AdvancingClock(Clock):
            def __call__(self):
                self.now += .0006
                return self.now
            def sleep(self, duration):
                if duration < 0:
                    raise ValueError('negative sleep')
                self.now += duration
        self.clock = AdvancingClock()
        result = self.run_send(Backend())
        self.assertTrue(result['complete'], result.get('error'))
        self.assertEqual(result['count'], 22)

    def test_phrase(self):
        rows = kit.phrase()
        self.assertEqual(len(rows), 22)
        self.assertEqual(kit.ownership(rows), [])
        self.assertEqual({e['semantic']['channel'] for e in rows}, {2, 3})
        self.assertEqual({e['semantic']['pitch14'] for e in rows if e['semantic']['type'] == 'pitchwheel'},
                         {9216, 7168, 10240, 6144, 8192})
        self.assertEqual(kit.compare(kit.log('expected', rows), kit.log('replay', rows))['result'], 'PASS')
    def test_confirm_before_access(self):
        b = Backend()
        def forbidden(*args):
            self.fail('inventory/open reached without confirmation')
        b.inventory = b.open = forbidden
        with self.assertRaises(ValueError):
            self.run_send(b, 'no')
    def test_missing_duplicate_uid(self):
        for rows in ([], [dict(name='owned', uid=8, direction='output')],
                     [dict(name='owned', uid=7, direction='output')] * 2):
            self.path = Path(self.tmp.name) / (str(id(rows)) + '.jsonl')
            b = Backend(); b.rows = rows
            with self.assertRaises(ValueError):
                self.run_send(b)
            self.assertEqual(b.opened, [])
    def test_success_no_broadcast_or_extra_cleanup(self):
        b = Backend()
        result = self.run_send(b)
        self.assertTrue(result['complete'])
        self.assertEqual(len(b.handle.sent), 22)
        self.assertEqual(b.handle.sent, [e['raw'] for e in kit.phrase()])
        self.assertTrue(b.handle.closed)
        self.assertEqual(len(b.opened), 1)
        self.assertEqual(kit.read_log(self.path), result)
    def test_sigint_sigterm_exception_cleanup(self):
        for exception in (KeyboardInterrupt('SIGINT'), KeyboardInterrupt('SIGTERM'), RuntimeError('failure')):
            self.path = Path(self.tmp.name) / (str(id(exception)) + '.jsonl')
            b = Backend()
            def interrupt():
                if self.clock.now >= .99:
                    raise exception
            self.clock = Clock(); self.clock.hook = interrupt
            result = self.run_send(b)
            self.assertFalse(result['complete'])
            self.assertTrue(b.handle.closed)
            self.assertIn([0x81, 57, 0], b.handle.sent)
            self.assertIn([0x82, 64, 0], b.handle.sent)
            self.assertTrue(all((raw[0] & 15) in (1, 2) for raw in b.handle.sent))
            self.assertTrue(any(e['phase'] == 'cleanup' for e in result['events']))
    def test_real_process_signals_cleanup(self):
        import os
        import signal
        for sig in (signal.SIGINT, signal.SIGTERM):
            b = Backend(); self.clock = Clock()
            self.path = Path(self.tmp.name) / (str(sig) + '.jsonl')
            previous = signal.signal(sig, kit.terminated)
            def kill_self():
                if self.clock.now >= .99:
                    os.kill(os.getpid(), sig)
            self.clock.hook = kill_self
            try:
                result = self.run_send(b)
            finally:
                signal.signal(sig, previous)
            self.assertFalse(result['complete'])
            self.assertIn([0x81, 57, 0], b.handle.sent)
            self.assertIn([0x82, 64, 0], b.handle.sent)
            self.assertTrue(b.handle.closed)
    def test_capture_cancel_fault_still_closes(self):
        b = Backend('input')
        def fail_cancel():
            raise RuntimeError('callback cancellation fault')
        b.handle.cancel_callback = fail_cancel
        result = kit.capture(b, 'owned', 7, self.path, 1, clock=self.clock,
                             sleep=self.clock.sleep, active_seconds=.5)
        self.assertFalse(result['complete'])
        self.assertTrue(b.handle.closed)
        self.assertEqual(b.handle.sent, [])
    def test_send_error_owns_uncertain_attack(self):
        b = Backend(); b.handle.fail_on = 6
        result = self.run_send(b)
        self.assertFalse(result['complete'])
        self.assertIn([0x81, 57, 0], b.handle.sent)
        self.assertTrue(b.handle.closed)
    def test_disconnect_no_new_output(self):
        b = Backend()
        self.clock.hook = lambda: b.rows.clear() if self.clock.now >= 1 else None
        result = self.run_send(b)
        self.assertFalse(result['complete'])
        self.assertTrue(result['cleanupErrors'])
        self.assertEqual(len(b.opened), 1)
        self.assertTrue(b.handle.closed)
    def test_changed_identity_after_open(self):
        b = Backend()
        original_open = b.open
        def changed_open(direction, selected):
            handle = original_open(direction, selected)
            b.rows[0] = dict(name='owned', uid=8, direction='output')
            return handle
        b.open = changed_open
        result = self.run_send(b)
        self.assertFalse(result['complete'])
        self.assertEqual(b.handle.sent, [])
        self.assertTrue(b.handle.closed)
    def test_macos_adapter_rejects_ambiguous_changed_and_open_error(self):
        from types import SimpleNamespace
        for mode in ('duplicate', 'reordered', 'driver_error'):
            b = kit.MacBackend.__new__(kit.MacBackend)
            endpoint = dict(name='owned', uid=7, direction='output')
            b.inventory = lambda direction: [endpoint]
            class RtHandle:
                def __init__(self):
                    self.closed, self.open_count, self.reads = False, 0, 0
                def get_ports(self):
                    self.reads += 1
                    if mode == 'duplicate': return ['owned', 'owned']
                    if mode == 'reordered' and self.reads > 1: return ['NEVER OPEN', 'owned']
                    return ['owned', 'NEVER OPEN']
                def open_port(self, index):
                    assert index == 0
                    self.open_count += 1
                    if mode == 'driver_error': raise RuntimeError('driver error')
                def close_port(self): self.closed = True
            handle = RtHandle()
            b.rt = SimpleNamespace(MidiOut=lambda: handle)
            with self.assertRaises((ValueError, RuntimeError)):
                b.open('output', endpoint)
            self.assertTrue(handle.closed)
            if mode == 'duplicate': self.assertEqual(handle.open_count, 0)
    def test_sender_late_deadline_aborts_without_burst(self):
        b = Backend()
        def late():
            self.clock.now += .2
        self.clock.hook = late
        result = self.run_send(b)
        self.assertFalse(result['complete'])
        self.assertEqual(len([e for e in result['events'] if e['phase'] == 'music']), 1)
        self.assertTrue(b.handle.closed)
    def test_existing_evidence_not_overwritten(self):
        b = Backend(); self.path.write_text('retain')
        with self.assertRaises(FileExistsError):
            self.run_send(b)
        self.assertEqual(b.opened, [])
        self.assertEqual(self.path.read_text(), 'retain')
    def test_capture_only_bounded_and_never_sends(self):
        b = Backend('input')
        def burst():
            if b.handle.callback:
                for _ in range(20):
                    b.handle.callback(([0x91, 57, 100], 0))
        self.clock.hook = burst
        result = kit.capture(b, 'owned', 7, self.path, 1, maximum=5,
                             clock=self.clock, sleep=self.clock.sleep, active_seconds=.5)
        self.assertFalse(result['complete'])
        self.assertEqual(len(result['events']), 5)
        self.assertEqual(b.handle.sent, [])
        self.assertTrue(b.handle.closed)
        self.assertEqual(b.opened[0][0], 'input')
    def test_capture_interrupt_disconnect_and_exception(self):
        for mode in ('interrupt', 'disconnect', 'exception'):
            b = Backend('input'); self.clock = Clock()
            self.path = Path(self.tmp.name) / (mode + '.jsonl')
            def fault():
                if mode == 'disconnect':
                    b.rows.clear()
                elif mode == 'interrupt':
                    raise KeyboardInterrupt('SIGTERM')
                else:
                    raise RuntimeError('input failure')
            self.clock.hook = fault
            result = kit.capture(b, 'owned', 7, self.path, 1,
                                 clock=self.clock, sleep=self.clock.sleep, active_seconds=.5)
            self.assertFalse(result['complete'])
            self.assertTrue(b.handle.closed)
            self.assertEqual(b.handle.sent, [])
    def test_capture_empty_and_invalid_duration(self):
        b = Backend('input')
        result = kit.capture(b, 'owned', 7, self.path, 1, clock=self.clock,
                             sleep=self.clock.sleep, active_seconds=.5)
        self.assertEqual(kit.compare(kit.log('expected', kit.phrase()), result)['result'], 'FAIL')
        for seconds in (float('nan'), float('inf'), -1, 31):
            with self.assertRaises(ValueError):
                kit.capture(b, 'owned', 7, self.path, seconds)
    def test_normalization_only_gate_encoding(self):
        self.assertEqual(kit.semantic([0x91, 57, 0]), kit.semantic([0x81, 57, 64]))
        self.assertNotEqual(kit.semantic([0x91, 57, 90]), kit.semantic([0x91, 57, 91]))
    def test_negative_controls(self):
        expected = kit.log('expected', kit.phrase())
        for mode in ('channel', 'expression', 'missing', 'extra', 'order', 'timing', 'empty', 'no_notes', 'truncated', 'count'):
            actual = copy.deepcopy(expected)
            rows = actual['events']
            if mode in ('channel', 'expression'):
                raw = rows[8]['raw'][:]
                if mode == 'channel': raw[0] = 0xE3
                else: raw[1] = 1
                rows[8] = kit.event(rows[8]['t'], raw)
            elif mode == 'missing': rows.pop(8)
            elif mode == 'extra': rows.insert(8, copy.deepcopy(rows[8]))
            elif mode == 'order':
                rows[8], rows[9] = rows[9], rows[8]
                rows[8]['t'], rows[9]['t'] = 1, 1.1
            elif mode == 'timing': rows[8]['t'] += .05
            elif mode == 'empty': rows.clear()
            elif mode == 'no_notes': rows[:] = [e for e in rows if not e['semantic']['type'].startswith('note_')]
            elif mode == 'truncated': actual['complete'] = False
            actual['count'] = len(rows) + (1 if mode == 'count' else 0)
            report = kit.compare(expected, actual)
            self.assertEqual(report['result'], 'FAIL', mode)
            if mode == 'timing':
                self.assertEqual(report['semantic'], 'PASS')
                self.assertEqual(report['timing'], 'FAIL')
    def test_global_alignment_not_event_alignment(self):
        a = kit.log('expected', kit.phrase()); b = copy.deepcopy(a)
        for row in b['events']: row['t'] += 10
        self.assertEqual(kit.compare(a, b)['result'], 'PASS')
        b['events'][8]['t'] += .05
        self.assertEqual(kit.compare(a, b)['result'], 'FAIL')
    def test_truncated_journal_fails(self):
        self.path.write_text('{"received":{}}\n')
        with self.assertRaises(ValueError): kit.read_log(self.path)
    def test_zero_event_final_journal_is_readable(self):
        # An output-open or input-open fault writes only a final record with count 0 and
        # no per-event lines. That package is a legitimate incomplete take, not truncation.
        for kind in ('sent', 'replay'):
            final = kit.log(kind, [], complete=False, error='RuntimeError: open fault',
                            endpoint=dict(name='owned', uid=7, direction='output'))
            path = Path(self.tmp.name) / (kind + '-openfault.jsonl')
            # Exactly what send/capture flush when the handle never opened: no event lines.
            path.write_text(json.dumps(dict(final=final)) + '\n')
            self.assertEqual(kit.read_log(path), final)
    def test_duplicate_final_record_is_named_not_called_missing(self):
        record = json.dumps(dict(final=kit.log('sent', [], complete=False)))
        self.path.write_text(record + '\n' + record + '\n')
        with self.assertRaises(ValueError) as raised:
            kit.read_log(self.path)
        self.assertIn('duplicate final record', str(raised.exception))
    def test_comparator_records_provenance_and_rejects_non_replay_actual(self):
        # A report must carry each journal's kind/endpoint, and the hardware-facing CLI path
        # must refuse to certify anything that is not a received replay capture. Without this
        # a same-file or sent-as-actual pairing reads PASS with no device-level warning.
        expected = kit.log('expected', kit.phrase())
        sent = kit.log('sent', kit.phrase(), endpoint=dict(name='owned', uid=7, direction='output'))
        replay = kit.log('replay', kit.phrase(), endpoint=dict(name='owned', uid=8, direction='input'))
        report = kit.compare(expected, replay, require_replay=True)
        self.assertEqual(report['result'], 'PASS')
        self.assertEqual(report['provenance'], dict(expectedKind='expected', actualKind='replay',
                                                   expectedEndpoint=None, actualEndpoint=replay['endpoint']))
        for actual in (expected, sent):
            report = kit.compare(expected, actual, require_replay=True)
            self.assertEqual(report['result'], 'FAIL')
            self.assertIn('not a replay capture', ' '.join(report['errors']))
            self.assertEqual(report['provenance']['actualKind'], actual['kind'])
        same = kit.compare(expected, expected)
        self.assertTrue(same['warnings'])
        self.assertEqual(same['provenance']['expectedKind'], same['provenance']['actualKind'])
        # The helper API only warns; the CLI gate (require_replay) is the refusal above.
        self.assertEqual(same['result'], 'PASS')
        self.assertEqual(kit.compare(expected, expected, require_replay=True)['result'], 'FAIL')
    def test_cli_compare_refuses_self_comparison(self):
        # Reproduced defect: `compare --expected X --actual X` exited 0 with PASS.
        kit_dir = Path(__file__).resolve().parent
        log = Path(self.tmp.name) / 'expected.json'
        kit.write_json(log, kit.log('expected', kit.phrase()))
        report = Path(self.tmp.name) / 'self-report.json'
        proc = subprocess.run([sys.executable, str(kit_dir / 'kit.py'), 'compare', '--expected', str(log),
                               '--actual', str(log), '--report', str(report)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(report.read_text())['result'], 'FAIL')
        both = subprocess.run([sys.executable, str(kit_dir / 'kit.py'), 'compare', '--expected', str(log),
                               '--actual', str(log), '--require-replay', '--report',
                               str(Path(self.tmp.name) / 'self2.json')], capture_output=True, text=True)
        self.assertEqual(both.returncode, 1)
    def test_optimized_interpreter_still_runs_acceptance_gates(self):
        # Regression: every selftest gate was a bare assert, so `python -O` skipped the
        # checks and still wrote result PASS. The gate helper must fail under -O.
        kit_dir = Path(__file__).resolve().parent
        fail = subprocess.run(
            [sys.executable, '-O', '-c', 'import selftest; selftest.check(False, "forced")'],
            cwd=kit_dir, capture_output=True, text=True)
        self.assertNotEqual(fail.returncode, 0)
        self.assertIn('FAIL: forced', fail.stderr + fail.stdout)
        ok = subprocess.run(
            [sys.executable, '-O', '-c', 'import selftest; selftest.check(True, "ok")'],
            cwd=kit_dir, capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0)
    def test_send_limits(self):
        for rows in ([kit.event(11, [0x91, 57, 100])],
                     [kit.event(0, [0x91, 57, 100]), kit.event(.001, [0x81, 57, 0])],
                     [kit.event(0, [0x90, 57, 100])]):
            b = Backend()
            with self.assertRaises(ValueError):
                kit.send(b, 'owned', 7, kit.CONFIRM, self.path, rows=rows)
            self.assertEqual(b.opened, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
