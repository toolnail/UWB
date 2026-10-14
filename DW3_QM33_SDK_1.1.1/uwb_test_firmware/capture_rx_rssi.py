"""Read existing text RTT channel 0 and complete RSSI channel 1 in ONE probe
session. No extra terminal output. Requires only Python and the installed SEGGER
DLL. Raw binary is retained; incomplete captures are explicitly rejected.
"""
import argparse
import csv
import ctypes
import json
import os
from pathlib import Path
import re
import struct
import time

WIRE = struct.Struct('<2sBBIIhh')


class RssiDecoder:
    def __init__(self, writer, expect_signal=True):
        self.writer = writer
        self.expect_signal = expect_signal
        self.pending = bytearray()
        self.start = self.end = None
        self.rows = self.unique = self.duplicates = self.invalid = 0
        self.last_id = 0
        self.sequences = set()
        writer.writerow(['sequence', 'reception_id', 'rssi_dbm', 'fp_power_dbm',
                         'diag_valid', 'rssi_valid', 'fp_valid', 'duplicate',
                         'rssi_q8', 'fp_power_q8'])

    def feed(self, data):
        self.pending.extend(data)
        length = len(self.pending) // WIRE.size * WIRE.size
        for magic, kind, flags, sequence, reception, rssi, fp in WIRE.iter_unpack(self.pending[:length]):
            if magic != b'RS':
                raise ValueError('RSSI stream corrupted or not the expected firmware format')
            if self.end is not None:
                raise ValueError('Records appeared after RSSI END marker')
            if kind == 0:
                if self.start is not None or self.rows:
                    raise ValueError('Repeated START: board reset or mixed runs')
                if reception != 1 or rssi != WIRE.size or fp != 0 or flags:
                    raise ValueError('Unsupported RSSI wire version')
                self.start = sequence
            elif kind == 1:
                if self.start is None or sequence >= self.start or reception <= self.last_id:
                    raise ValueError('Missing START or invalid sequence/reception ordering')
                if flags & ~7:
                    raise ValueError('Unsupported RSSI flags')
                duplicate = bool(flags & 4)
                if duplicate != (sequence in self.sequences):
                    raise ValueError('Duplicate flag disagrees with sequence history')
                self.sequences.add(sequence)
                rv, fv = bool(flags & 1), bool(flags & 2)
                if rv != (rssi != -32768) or fv != (fp != -32768):
                    raise ValueError('Invalid RSSI sentinel/flags')
                self.writer.writerow([sequence, reception,
                    f'{rssi / 256:.8f}' if rv else 'NA',
                    f'{fp / 256:.8f}' if fv else 'NA', int(rv and fv),
                    int(rv), int(fv), int(duplicate), rssi, fp])
                self.rows += 1
                self.duplicates += int(duplicate)
                self.unique += int(not duplicate)
                self.invalid += int(not duplicate and not (rv and fv))
                self.last_id = reception
            elif kind == 2:
                if self.start is None or flags:
                    raise ValueError('END without START')
                dropped = (rssi & 0xffff) | ((fp & 0xffff) << 16)
                self.end = {'unique': sequence, 'generated': reception, 'dropped': dropped}
            else:
                raise ValueError('Unknown RSSI record type')
        del self.pending[:length]

    def finish(self, log_text):
        if self.pending or self.start is None or self.end is None:
            raise ValueError('Incomplete RSSI capture: missing START/END or partial record')
        if self.end['dropped'] or self.rows != self.end['generated'] or self.unique != self.end['unique']:
            raise ValueError(f'RSSI records were lost: rows={self.rows}, footer={self.end}')
        complete = re.findall(r'^RX_COMPLETE (.+)$', log_text, re.M)
        signals = re.findall(r'^RX_SIGNAL_COMPLETE (.+)$', log_text, re.M)
        if len(complete) != 1 or len(signals) != int(self.expect_signal):
            raise ValueError('Missing or mixed complete text log')
        packet = dict(re.findall(r'(\w+)=(\S+)', complete[0]))
        if (int(packet['target']) != self.start or int(packet['success']) != self.unique or
            int(packet['dup']) != self.duplicates):
            raise ValueError('Full RSSI file counts disagree with firmware summary')
        if self.expect_signal:
            signal = dict(re.findall(r'(\w+)=(\S+)', signals[0]))
            if (int(signal['samples']) + int(signal['invalid']) != self.unique or
                int(signal['invalid']) != self.invalid):
                raise ValueError('Full RSSI validity counts disagree with firmware summary')
        elif self.invalid != self.unique:
            raise ValueError('Signal-disabled capture unexpectedly contains valid unique RSSI')
        return {'complete': True, 'target': self.start, 'rows': self.rows,
                'unique': self.unique, 'duplicates': self.duplicates,
                'invalid_unique': self.invalid, 'stream_dropped': self.end['dropped']}


class JLinkRtt:
    """Minimal typed bindings to SEGGER J-Link SDK RTT APIs. No reset/flash."""
    def __init__(self, directory, serial, address, speed, diagnostics):
        dll_name = 'JLink_x64.dll' if struct.calcsize('P') == 8 else 'JLinkARM.dll'
        self.dll = ctypes.CDLL(str(Path(directory) / dll_name))
        self.opened = False
        callback = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
        def report(message):
            diagnostics.write(message.decode('utf-8', 'replace') + '\n')
            diagnostics.flush()
        self.log_callback = callback(report)
        self.error_callback = callback(report)
        signatures = {
            'JLINKARM_EMU_SelectByUSBSN': ([ctypes.c_uint32], ctypes.c_int),
            'JLINKARM_OpenEx': ([callback, callback], ctypes.c_char_p),
            'JLINKARM_ExecCommand': ([ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int], ctypes.c_int),
            'JLINKARM_TIF_Select': ([ctypes.c_int], ctypes.c_int),
            'JLINKARM_SetSpeed': ([ctypes.c_uint32], None),
            'JLINKARM_Connect': ([], ctypes.c_int),
            'JLINKARM_Go': ([], None), 'JLINKARM_Close': ([], None),
            'JLINK_RTTERMINAL_Control': ([ctypes.c_uint32, ctypes.c_void_p], ctypes.c_int),
            'JLINK_RTTERMINAL_Read': ([ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32], ctypes.c_int),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.dll, name)
            function.argtypes, function.restype = args, result
        try:
            if self.dll.JLINKARM_EMU_SelectByUSBSN(serial) < 0:
                raise RuntimeError(f'J-Link probe {serial} not found')
            error = self.dll.JLINKARM_OpenEx(self.log_callback, self.error_callback)
            if error:
                raise RuntimeError(error.decode('utf-8', 'replace'))
            self.opened = True
            errors = ctypes.create_string_buffer(512)
            if self.dll.JLINKARM_ExecCommand(b'Device = Cortex-M4', errors, len(errors)) < 0:
                raise RuntimeError(errors.value.decode('utf-8', 'replace'))
            if self.dll.JLINKARM_TIF_Select(1) < 0:
                raise RuntimeError('Cannot select SWD')
            # Conservative connection handshake, then faster background transfer.
            self.dll.JLINKARM_SetSpeed(100)
            if self.dll.JLINKARM_Connect() < 0:
                raise RuntimeError('Cannot connect to RX target')
            self.dll.JLINKARM_SetSpeed(speed)
            self.dll.JLINKARM_Go()
            config = (ctypes.c_uint32 * 4)(address, 0, 0, 0)
            if self.dll.JLINK_RTTERMINAL_Control(0, config) < 0:
                raise RuntimeError('Cannot start RTT')
            self.buffer = ctypes.create_string_buffer(65536)
        except BaseException:
            self.close()
            raise

    def read(self, channel):
        count = self.dll.JLINK_RTTERMINAL_Read(channel, self.buffer, len(self.buffer))
        if count < 0:
            raise RuntimeError(f'RTT channel {channel} read failed: {count}')
        return self.buffer.raw[:count]

    def ready(self):
        direction = ctypes.c_int(0)
        return self.dll.JLINK_RTTERMINAL_Control(3, ctypes.byref(direction)) >= 2

    def close(self):
        if self.opened:
            self.dll.JLINK_RTTERMINAL_Control(1, None)
            self.dll.JLINKARM_Close()
            self.opened = False


def capture(args):
    prefix = Path(args.prefix)
    stop = Path(str(prefix) + '.capture.stop')
    ready_path = Path(str(prefix) + '.capture.ready.json')
    status = Path(str(prefix) + '.rx.rssi.capture.json')
    temporary = Path(str(prefix) + '.rx.rssi.partial.csv')
    log_path = Path(args.text_log) if args.text_log else Path(str(prefix) + '.rx.log')
    link = None
    try:
        with (open(str(prefix) + '.rx_status.log', 'w', encoding='utf-8') as diagnostics,
              open(log_path, 'wb') as text_log,
              open(str(prefix) + '.rx.rssi.bin', 'wb') as binary,
              open(temporary, 'w', newline='', encoding='utf-8-sig') as csv_file):
            decoder = RssiDecoder(csv.writer(csv_file), expect_signal=bool(args.signal_enabled))
            link = JLinkRtt(args.jlink_directory, args.serial, args.address, args.speed, diagnostics)
            deadline = time.monotonic() + 40
            while not link.ready():
                if time.monotonic() >= deadline or stop.exists():
                    raise RuntimeError('RTT control block not found')
                time.sleep(0.02)
            last_flush = time.monotonic()
            quiet_since = None
            while True:
                # Binary first so long-running text consumers cannot starve it.
                data = link.read(1)
                text = link.read(0)
                if data:
                    binary.write(data)
                    decoder.feed(data)
                    if decoder.start is not None and not ready_path.exists():
                        ready_tmp = Path(str(ready_path) + '.tmp')
                        ready_tmp.write_text(json.dumps({'target': decoder.start, 'version': 1}), encoding='utf-8')
                        os.replace(ready_tmp, ready_path)
                if text:
                    text_log.write(text)
                now = time.monotonic()
                if now - last_flush >= 0.1:
                    text_log.flush(); binary.flush(); csv_file.flush()
                    last_flush = now
                if stop.exists():
                    quiet_since = now if quiet_since is None or data or text else quiet_since
                    if decoder.end is not None and now - quiet_since >= 0.25:
                        break
                    if time.time() - stop.stat().st_mtime > 5:
                        raise RuntimeError('Timed out draining RSSI stream; file is incomplete')
                if not data and not text:
                    time.sleep(0.002)
            text_log.flush(); binary.flush(); csv_file.flush()
            summary = decoder.finish(log_path.read_text(encoding='utf-8', errors='replace'))
        os.replace(temporary, str(prefix) + '.rx.rssi.csv')
        status.write_text(json.dumps(summary, indent=2), encoding='utf-8')
    except BaseException as error:
        status.write_text(json.dumps({'complete': False, 'error': str(error)}, indent=2), encoding='utf-8')
        raise
    finally:
        if link is not None:
            link.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--serial', type=int, required=True)
    parser.add_argument('--address', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--speed', type=int, default=1000)
    parser.add_argument('--jlink-directory', required=True)
    parser.add_argument('--text-log')
    parser.add_argument('--signal-enabled', type=int, choices=(0, 1), default=1)
    capture(parser.parse_args())
