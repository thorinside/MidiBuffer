#!/usr/bin/env python3
"""Bounded MIDI 1 lower-zone preparation kit. No MIDI handles on import."""
import argparse
import ctypes as C
import json
import math
from pathlib import Path
import signal
import sys
import time

MAX_EVENTS = 4096
MAX_SECONDS = 30
CONFIRM = 'I_AUTHORIZE_PHYSICAL_MIDI_TEST'


def semantic(raw):
    import mido
    msg = mido.Message.from_bytes(raw)
    d = msg.dict()
    d.pop('time')
    if 'channel' in d:
        d['channel'] += 1  # public logs always MIDI 1 channels 1..16
    if d['type'] == 'pitchwheel':
        d['pitch14'] = d.pop('pitch') + 8192
    if d['type'] == 'note_on' and d['velocity'] == 0:
        d['type'] = 'note_off'
    if d['type'] == 'note_off':
        d.pop('velocity')  # gate semantics; raw release velocity is retained
    return d


def event(t, raw, phase='music'):
    return dict(t=round(t, 9), raw=list(raw), semantic=semantic(list(raw)), phase=phase)


def phrase():
    # Fixed 2-member lower zone. No MCM/RPN/setup; every expression is music.
    rows = [(0, [0xE1, 0, 72]), (.05, [0xD1, 20]), (.10, [0xB1, 74, 32]),
            (.15, [0xE2, 0, 56]), (.20, [0xD2, 40]), (.25, [0xB2, 74, 96]),
            (.50, [0x91, 57, 100]), (.75, [0x92, 64, 90]),
            (1, [0xE1, 0, 80]), (1.10, [0xE2, 0, 48]),
            (1.20, [0xD1, 100]), (1.30, [0xD2, 60]),
            (1.40, [0xB1, 74, 10]), (1.50, [0xB2, 74, 120]),
            (2, [0x81, 57, 64]), (2.25, [0x82, 64, 32]),
            (2.40, [0xE1, 0, 64]), (2.45, [0xE2, 0, 64]),
            (2.50, [0xD1, 0]), (2.55, [0xD2, 0]),
            (2.60, [0xB1, 74, 64]), (2.65, [0xB2, 74, 64])]
    return [event(t, raw) for t, raw in rows]


def write_json(path, data):
    # Refuse overwriting evidence. Parent directory may be created by the caller.
    with Path(path).open('x') as f:
        json.dump(data, f, indent=2, allow_nan=False)
        f.write('\n')


def log(kind, events, complete=True, **extra):
    return dict(version=1, kind=kind, complete=complete, count=len(events), events=events, **extra)


def validate(data):
    if data.get('version') != 1 or data.get('complete') is not True:
        raise ValueError('incomplete/truncated/unsupported log')
    rows = data['events']
    if data['count'] != len(rows) or not 0 < len(rows) <= MAX_EVENTS:
        raise ValueError('empty/count/bound violation')
    prev = -1
    for i, e in enumerate(rows):
        if not isinstance(e['t'], (float, int)) or not math.isfinite(e['t']) or e['t'] < prev:
            raise ValueError(f'index {i}: timestamp invalid or reordered')
        prev = e['t']
        if e['semantic'] != semantic(e['raw']) or e['phase'] not in ('music', 'replay', 'cleanup', 'setup', 'context'):
            raise ValueError(f'index {i}: raw/semantic/phase disagreement')
    return rows


def ownership(rows):
    held = set()
    errors = []
    notes = 0
    for i, e in enumerate(rows):
        s = e['semantic']
        if s['type'] not in ('note_on', 'note_off'):
            continue
        key = s['channel'], s['note']
        if s['type'] == 'note_on':
            notes += 1
            if key in held:
                errors.append(f'index {i}: repeated owned attack {key}')
            held.add(key)
        elif key not in held:
            errors.append(f'index {i}: unowned ending {key}')
        else:
            held.remove(key)
    if not notes:
        errors.append('no note attacks')
    if held:
        errors.append(f'unreleased owned notes {sorted(held)}')
    return errors


def provenance(expected, actual):
    """Journal identity recorded in every report so a paired-file mistake is visible."""
    def field(data, key):
        return data.get(key) if isinstance(data, dict) else None
    return dict(expectedKind=field(expected, 'kind'), actualKind=field(actual, 'kind'),
                expectedEndpoint=field(expected, 'endpoint'), actualEndpoint=field(actual, 'endpoint'))


def compare(expected, actual, tolerance=.025, tempo=1, require_replay=False):
    if not math.isfinite(tolerance) or not 0 <= tolerance <= .5 or not math.isfinite(tempo) or not .25 <= tempo <= 4:
        raise ValueError('tolerance must be 0..0.5 seconds; tempo scale 0.25..4')
    ident = provenance(expected, actual)
    warnings = []
    if ident['expectedKind'] == ident['actualKind'] and ident['expectedEndpoint'] == ident['actualEndpoint']:
        warnings.append('expected and actual share kind and endpoint; not independent received-device evidence')
    errors, timing = [], []
    try:
        a, b = validate(expected), validate(actual)
    except (KeyError, TypeError, ValueError) as ex:
        return dict(result='FAIL', semantic='FAIL', timing='NOT_ASSESSED', errors=[str(ex)],
                    provenance=ident, warnings=warnings)
    # The hardware acceptance path must compare against an actual received capture.
    # A sent/replay log pair is fine; an expected-vs-expected or sent-as-actual pair is not.
    if require_replay and ident['actualKind'] != 'replay':
        errors.append(f"actual journal kind {ident['actualKind']!r} is not a replay capture")
    errors += ['expected: ' + x for x in ownership(a)]
    errors += ['actual: ' + x for x in ownership(b)]
    if len(a) != len(b):
        errors.append(f'event count expected={len(a)} actual={len(b)}')
    offset = b[0]['t'] - a[0]['t'] * tempo
    for i in range(max(len(a), len(b))):
        x, y = a[i] if i < len(a) else None, b[i] if i < len(b) else None
        if x is None or y is None or x['semantic'] != y['semantic']:
            errors.append(dict(index=i, expected=x and x['semantic'], actual=y and y['semantic']))
        if x is not None and y is not None:
            delta = y['t'] - (x['t'] * tempo + offset)
            if abs(delta) > tolerance:
                timing.append(dict(index=i, channel=y['semantic'].get('channel'), type=y['semantic']['type'],
                                   expected=x['t'] * tempo + offset, actual=y['t'], errorSeconds=delta))
    return dict(result='FAIL' if errors or timing else 'PASS', semantic='FAIL' if errors else 'PASS',
                timing='FAIL' if timing else 'PASS', errors=errors, timingErrors=timing,
                globalOffsetSeconds=offset, toleranceSeconds=tolerance, tempoScale=tempo,
                expectedCount=len(a), actualCount=len(b), exclusions=[],
                provenance=ident, warnings=warnings)


class MacBackend:
    """Read-only CoreMIDI endpoint UID inventory, exact RtMidi handle selection."""
    def __init__(self):
        import rtmidi
        self.rt = rtmidi
        self.cm = C.CDLL('/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI')
        self.cf = C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        self.cf.CFStringGetCString.argtypes = [C.c_void_p, C.c_void_p, C.c_long, C.c_uint32]
        self.cm.MIDIObjectGetStringProperty.argtypes = [C.c_uint32, C.c_void_p, C.POINTER(C.c_void_p)]
        self.cm.MIDIObjectGetIntegerProperty.argtypes = [C.c_uint32, C.c_void_p, C.POINTER(C.c_int32)]
        self.cf.CFRelease.argtypes = [C.c_void_p]
        for name in ('MIDIGetSource', 'MIDIGetDestination'):
            getattr(self.cm, name).restype = C.c_uint32
        for name in ('MIDIGetNumberOfSources', 'MIDIGetNumberOfDestinations'):
            getattr(self.cm, name).restype = C.c_ulong

    def inventory(self, direction):
        is_out = direction == 'output'
        count = self.cm.MIDIGetNumberOfDestinations if is_out else self.cm.MIDIGetNumberOfSources
        get = self.cm.MIDIGetDestination if is_out else self.cm.MIDIGetSource
        uidkey = C.c_void_p.in_dll(self.cm, 'kMIDIPropertyUniqueID')
        namekey = C.c_void_p.in_dll(self.cm, 'kMIDIPropertyDisplayName')
        rows = []
        for i in range(count()):
            ref, uid, name = get(i), C.c_int32(), C.c_void_p()
            if self.cm.MIDIObjectGetIntegerProperty(ref, uidkey, C.byref(uid)) != 0 or self.cm.MIDIObjectGetStringProperty(ref, namekey, C.byref(name)) != 0:
                raise ValueError('CoreMIDI metadata unavailable; refusing selection')
            try:
                buf = C.create_string_buffer(1024)
                if not self.cf.CFStringGetCString(name, buf, 1024, 0x08000100):
                    raise ValueError('endpoint name too long')
                rows.append(dict(name=buf.value.decode(), uid=uid.value, direction=direction))
            finally:
                self.cf.CFRelease(name)
        return rows

    def check(self, selected):
        ref, kind = C.c_uint32(), C.c_int32()
        self.cm.MIDIObjectFindByUniqueID.argtypes = [C.c_int32, C.POINTER(C.c_uint32), C.POINTER(C.c_int32)]
        if self.cm.MIDIObjectFindByUniqueID(selected['uid'], C.byref(ref), C.byref(kind)) != 0:
            raise ValueError('selected endpoint disconnected')
        expected_kind = 3 if selected['direction'] == 'output' else 2
        if kind.value != expected_kind:
            raise ValueError('endpoint direction changed')
        name = C.c_void_p()
        key = C.c_void_p.in_dll(self.cm, 'kMIDIPropertyDisplayName')
        if self.cm.MIDIObjectGetStringProperty(ref, key, C.byref(name)) != 0:
            raise ValueError('selected endpoint metadata unavailable')
        try:
            buf = C.create_string_buffer(1024)
            if not self.cf.CFStringGetCString(name, buf, 1024, 0x08000100) or buf.value.decode() != selected['name']:
                raise ValueError('selected endpoint name changed')
        finally:
            self.cf.CFRelease(name)
        return selected

    def open(self, direction, selected):
        obj = self.rt.MidiOut() if direction == 'output' else self.rt.MidiIn(queue_size_limit=MAX_EVENTS)
        try:
            names = obj.get_ports()
            hits = [i for i, n in enumerate(names) if n == selected['name']]
            if len(hits) != 1 or select(self, direction, selected['name'], selected['uid']) != selected:
                raise ValueError('RtMidi/CoreMIDI identity missing, duplicated or changed')
            obj.open_port(hits[0])
            if obj.get_ports() != names or select(self, direction, selected['name'], selected['uid']) != selected:
                raise ValueError('endpoint inventory changed during open')
            if direction == 'input':
                obj.ignore_types(sysex=False, timing=False, active_sense=False)
            return obj
        except BaseException:
            obj.close_port()
            raise


def check_selected(backend, selected):
    if hasattr(backend, 'check'):
        return backend.check(selected)
    return select(backend, selected['direction'], selected['name'], selected['uid'])


def select(backend, direction, name, uid):
    rows = backend.inventory(direction)
    hits = [r for r in rows if r['name'] == name]
    if len(hits) != 1 or hits[0]['uid'] != uid:
        raise ValueError(f'{direction} absent/ambiguous/changed: {name!r} UID {uid}')
    return hits[0]


def send(backend, name, uid, confirmation, path, rows=None, sleep=time.sleep, clock=time.monotonic):
    if confirmation != CONFIRM:
        raise ValueError('explicit physical-test confirmation required before output access')
    rows = phrase() if rows is None else rows
    validate(log('expected', rows))
    if rows[-1]['t'] > 10 or any(b['t'] - a['t'] < .005 for a, b in zip(rows, rows[1:])):
        raise ValueError('send duration/rate exceeds 10s/200 events per second')
    if any(e['phase'] != 'music' or e['semantic'].get('channel') not in (2, 3) or
           e['semantic']['type'] not in ('note_on', 'note_off', 'pitchwheel', 'aftertouch', 'control_change') or
           (e['semantic']['type'] == 'control_change' and e['semantic']['control'] != 74) for e in rows):
        raise ValueError('generator scope violation')
    # Reserve evidence before opening any handle; stream every attempt durably.
    with Path(path).open('x') as journal:
        selected = select(backend, 'output', name, uid)
        output = None
        held, expression, sent, cleanup_errors = set(), {}, [], []
        complete, error = False, None
        start = clock()
        last_music_send = None
        def emit(raw, phase):
            e = event(clock() - start, raw, phase)
            journal.write(json.dumps(dict(attempt=e)) + '\n'); journal.flush()
            output.send_message(raw)
            sent.append(e)
            journal.write(json.dumps(dict(sent=e)) + '\n'); journal.flush()
        try:
            output = backend.open('output', selected)
            start = clock()
            for e in rows:
                deadline = start + e['t']
                while True:
                    remaining = deadline - clock()
                    if remaining <= 0:
                        break
                    sleep(min(.001, remaining))
                if clock() - deadline > .025:
                    raise RuntimeError('sender missed event deadline by more than 25ms; take incomplete')
                if last_music_send is not None and clock() - last_music_send < .005:
                    raise RuntimeError('actual music rate would exceed 200 events/second')
                if check_selected(backend, selected) != selected:
                    raise ValueError('disconnect/identity change')
                raw, s = e['raw'], e['semantic']
                # Own attempted note-ons too: a driver error may occur after dispatch.
                if s['type'] in ('pitchwheel', 'aftertouch', 'control_change'):
                    expression[s['channel'], s['type']] = s
                if s['type'] == 'note_on':
                    held.add((s['channel'], s['note']))
                emit(raw, 'music')
                last_music_send = clock()
                if s['type'] == 'note_off':
                    held.discard((s['channel'], s['note']))
            complete = True
        except BaseException as ex:
            error = f'{type(ex).__name__}: {ex}'
        finally:
            if output is not None:
                releases = [[0x80 | (ch - 1), n, 0] for ch, n in sorted(held)]
                for (ch, kind), state in sorted(expression.items()):
                    status = ch - 1
                    if kind == 'pitchwheel' and state['pitch14'] != 8192:
                        releases.append([0xE0 | status, 0, 64])
                    elif kind == 'aftertouch' and state['value'] != 0:
                        releases.append([0xD0 | status, 0])
                    elif kind == 'control_change' and state['value'] != 64:
                        releases.append([0xB0 | status, 74, 64])
                for raw in releases:
                    try:
                        check_selected(backend, selected)
                        emit(raw, 'cleanup')
                    except BaseException as ex:
                        cleanup_errors.append(str(ex))
                try:
                    output.close_port()
                except BaseException as ex:
                    cleanup_errors.append('close: ' + str(ex))
            result = log('sent', sent, complete and not cleanup_errors, error=error,
                         cleanupErrors=cleanup_errors, endpoint=selected)
            journal.write(json.dumps(dict(final=result)) + '\n'); journal.flush()
    return result


def capture(backend, name, uid, path, seconds, maximum=MAX_EVENTS, clock=time.monotonic,
            sleep=time.sleep, active_seconds=2.8):
    import threading
    if (not math.isfinite(seconds) or not 0 < seconds <= MAX_SECONDS or
            not math.isfinite(active_seconds) or not 0 < active_seconds <= seconds or
            not 1 <= maximum <= MAX_EVENTS):
        raise ValueError('capture limits: 0..30s, active <= arm deadline, 1..4096 events')
    with Path(path).open('x') as journal:
        selected = select(backend, 'input', name, uid)
        rows, pending, faults = [], [], []
        lock = threading.Lock()
        inp = None
        complete, error = False, None
        start = clock()
        first = [None]
        def receive(message, user_data=None):
            raw, _ = message
            now = clock()
            with lock:
                if faults:
                    return
                if first[0] is None:
                    first[0] = now
                if len(rows) + len(pending) >= maximum or len(raw) > 1024:
                    faults.append('capture event/packet bound reached; truncated')
                    return
                try:
                    pending.append(event(now - start, raw, 'replay'))
                except Exception as ex:
                    faults.append('invalid MIDI: ' + str(ex))
        def drain():
            with lock:
                for e in pending:
                    rows.append(e)
                    journal.write(json.dumps(dict(received=e)) + '\n')
                pending.clear()
                journal.flush()
                if faults:
                    raise ValueError(faults[0])
        try:
            inp = backend.open('input', selected)
            start = clock()
            inp.set_callback(receive)
            print('ARMED replay-only capture; start NT playback now', flush=True)
            while clock() - start < seconds:
                check_selected(backend, selected)
                drain()
                if first[0] is not None and clock() - first[0] >= active_seconds:
                    break
                sleep(.001)
            # A complete empty capture still fails comparator ownership/count checks.
            complete = True
        except BaseException as ex:
            error = f'{type(ex).__name__}: {ex}'
        finally:
            if inp is not None:
                try:
                    inp.cancel_callback()
                except BaseException as ex:
                    error, complete = 'cancel: ' + str(ex), False
                try:
                    inp.close_port()
                except BaseException as ex:
                    error, complete = 'close: ' + str(ex), False
            try:
                drain()
            except ValueError as ex:
                error, complete = str(ex), False
            result = log('replay', rows, complete, error=error, endpoint=selected,
                         activeSeconds=active_seconds, armDeadlineSeconds=seconds)
            journal.write(json.dumps(dict(final=result)) + '\n'); journal.flush()
    return result


def read_log(path):
    if Path(path).stat().st_size > 8_000_000:
        raise ValueError('log exceeds 8 MB')
    text = Path(path).read_text()
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        data = None
    # A whole-file JSON object is a supported single-log form only when it carries events.
    # A legitimate zero-event final journal also parses as one object; fall through to the
    # JSONL branch instead of rejecting it as truncated.
    if isinstance(data, dict) and 'events' in data:
        return data
    try:
        records = [json.loads(line) for line in text.splitlines() if line.strip()]
    except json.JSONDecodeError as ex:
        raise ValueError('malformed journal record: ' + str(ex))
    if not records or not all(isinstance(r, dict) for r in records):
        raise ValueError('missing final record; truncated journal')
    finals = [r for r in records if 'final' in r]
    if not finals:
        raise ValueError('missing final record; truncated journal')
    if len(finals) > 1 or 'final' not in records[-1]:
        raise ValueError('duplicate final record or final not last; truncated journal')
    result = records[-1]['final']
    field = 'received' if result['kind'] == 'replay' else 'sent'
    if [r[field] for r in records if field in r] != result['events']:
        raise ValueError('journal/final event disagreement')
    return result


def terminated(signum, frame):
    raise KeyboardInterrupt(f'signal {signum}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('list-ports')
    d = sub.add_parser('dry-run'); d.add_argument('--log', required=True)
    s = sub.add_parser('send')
    s.add_argument('--output', required=True); s.add_argument('--uid', type=int, required=True)
    s.add_argument('--confirm', required=True); s.add_argument('--log', required=True)
    c = sub.add_parser('capture')
    c.add_argument('--input', required=True); c.add_argument('--uid', type=int, required=True)
    c.add_argument('--seconds', type=float, default=15); c.add_argument('--active-seconds', type=float, default=2.8)
    c.add_argument('--log', required=True)
    r = sub.add_parser('compare')
    r.add_argument('--expected', required=True); r.add_argument('--actual', required=True)
    r.add_argument('--report', required=True); r.add_argument('--tolerance-ms', type=float, default=25)
    r.add_argument('--tempo-scale', type=float, default=1)
    r.add_argument('--require-replay', action='store_true',
                   help='fail unless the actual journal is a received replay capture (hardware acceptance)')
    a = p.parse_args()
    signal.signal(signal.SIGTERM, terminated)
    try:
        if a.command == 'dry-run':
            result = log('expected', phrase()); write_json(a.log, result)
        elif a.command == 'list-ports':
            backend = MacBackend()
            result = {d: backend.inventory(d) for d in ('input', 'output')}
        elif a.command == 'send':
            if a.confirm != CONFIRM:
                raise ValueError('explicit physical-test confirmation required')
            result = send(MacBackend(), a.output, a.uid, a.confirm, a.log)
        elif a.command == 'capture':
            result = capture(MacBackend(), a.input, a.uid, a.log, a.seconds, active_seconds=a.active_seconds)
        else:
            try:
                # The reproduced F3 defect is a self-comparison reading PASS: refuse the same file.
                if Path(a.expected).resolve() == Path(a.actual).resolve():
                    raise ValueError('expected and actual are the same file; refusing self-comparison')
                result = compare(read_log(a.expected), read_log(a.actual), a.tolerance_ms / 1000,
                                 a.tempo_scale, require_replay=a.require_replay)
            except (ValueError, OSError, KeyError, TypeError) as ex:
                result = dict(result='FAIL', semantic='FAIL', timing='NOT_ASSESSED', errors=[str(ex)])
            write_json(a.report, result)
        print(json.dumps(result, indent=2))
        return 1 if result.get('result') == 'FAIL' or result.get('complete') is False else 0
    except (ValueError, OSError, RuntimeError, KeyError) as ex:
        print('FAIL: ' + str(ex), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
