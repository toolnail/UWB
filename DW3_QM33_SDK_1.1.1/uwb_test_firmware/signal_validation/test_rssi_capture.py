import csv
import io
from pathlib import Path
import random
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_rx_rssi
from capture_rx_rssi import RssiDecoder, WIRE


def marker(kind, sequence, count, value):
    lo, hi = value & 0xffff, (value >> 16) & 0xffff
    return WIRE.pack(b'RS', kind, 0, sequence, count,
                     lo if lo < 32768 else lo - 65536,
                     hi if hi < 32768 else hi - 65536)


def log(unique, duplicate=0, invalid=0):
    return (f'RX_COMPLETE target=300000 success={unique} dup={duplicate}\n'
            f'RX_SIGNAL_COMPLETE samples={unique-invalid} invalid={invalid}\n')


class CaptureTests(unittest.TestCase):
    def decoder(self):
        out = io.StringIO()
        return out, RssiDecoder(csv.writer(out))

    def test_split_chunks_duplicates_invalid(self):
        out, decoder = self.decoder()
        wire = marker(0, 300000, 1, 16)
        wire += WIRE.pack(b'RS', 1, 3, 9, 10, -20672, -21000)
        wire += WIRE.pack(b'RS', 1, 7, 9, 11, -20000, -22000)
        wire += WIRE.pack(b'RS', 1, 0, 3, 14, -32768, -32768)
        wire += marker(2, 2, 3, 0)
        for value in wire:
            decoder.feed(bytes([value]))
        result = decoder.finish(log(2, 1, 1))
        self.assertEqual(result['rows'], 3)
        rows = list(csv.DictReader(io.StringIO(out.getvalue())))
        self.assertEqual([r['sequence'] for r in rows], ['9', '9', '3'])
        self.assertEqual(rows[0]['rssi_dbm'], '-80.75000000')
        self.assertEqual(rows[1]['duplicate'], '1')
        self.assertEqual(rows[2]['rssi_dbm'], 'NA')

    def test_missing_record_rejected(self):
        _, decoder = self.decoder()
        decoder.feed(marker(0, 300000, 1, 16) + marker(2, 1, 1, 1))
        with self.assertRaises(ValueError):
            decoder.finish(log(1))

    def test_partial_reset_corrupt_rejected(self):
        for data in (marker(0, 300000, 1, 16) + b'R',
                     marker(0, 300000, 1, 16) * 2,
                     b'XX' + marker(0, 300000, 1, 16)[2:]):
            _, decoder = self.decoder()
            with self.assertRaises(ValueError):
                decoder.feed(data)
                decoder.finish(log(0))

    def test_summary_mismatch_rejected(self):
        _, decoder = self.decoder()
        decoder.feed(marker(0, 300000, 1, 16) + marker(2, 0, 0, 0))
        with self.assertRaises(ValueError):
            decoder.finish(log(1))

    def test_300000_records(self):
        out, decoder = self.decoder()
        decoder.feed(marker(0, 300000, 1, 16))
        random.seed(1)
        for base in range(0, 300000, 1000):
            chunk = b''.join(WIRE.pack(b'RS', 1, 3, i, i+1, -20000 + i % 1000, -22000)
                             for i in range(base, base + 1000))
            while chunk:
                size = random.randrange(1, 4000)
                decoder.feed(chunk[:size]); chunk = chunk[size:]
        decoder.feed(marker(2, 300000, 300000, 0))
        self.assertEqual(decoder.finish(log(300000))['rows'], 300000)
        self.assertEqual(len(out.getvalue().splitlines()), 300001)

    def test_explicitly_disabled_signal(self):
        out = io.StringIO()
        decoder = RssiDecoder(csv.writer(out), expect_signal=False)
        decoder.feed(marker(0, 300000, 1, 16) +
                     WIRE.pack(b'RS', 1, 0, 0, 1, -32768, -32768) +
                     marker(2, 1, 1, 0))
        result = decoder.finish('RX_COMPLETE target=300000 success=1 dup=0\n')
        self.assertEqual(result['invalid_unique'], 1)

    def test_files_finalized_only_when_complete(self):
        for dropped in (0, 1):
            with self.subTest(dropped=dropped), tempfile.TemporaryDirectory() as folder:
                prefix = str(Path(folder) / 'round_1')
                Path(prefix + '.capture.stop').write_text('stop')
                wire = marker(0, 300000, 1, 16)
                wire += WIRE.pack(b'RS', 1, 3, 0, 1, -20672, -21000)
                wire += marker(2, 1, 1, dropped)
                class FakeRtt:
                    def __init__(self, *args):
                        self.payloads = {0: log(1).encode(), 1: wire}
                    def ready(self): return True
                    def read(self, channel): return self.payloads.pop(channel, b'')
                    def close(self): pass
                args = SimpleNamespace(prefix=prefix, text_log=None, signal_enabled=1,
                    serial=0, address=0, speed=1000, jlink_directory='unused')
                with patch.object(capture_rx_rssi, 'JLinkRtt', FakeRtt):
                    if dropped:
                        with self.assertRaises(ValueError): capture_rx_rssi.capture(args)
                    else:
                        capture_rx_rssi.capture(args)
                self.assertEqual(Path(prefix + '.rx.rssi.csv').exists(), not dropped)
                self.assertEqual(Path(prefix + '.rx.rssi.partial.csv').exists(), bool(dropped))


if __name__ == '__main__':
    unittest.main()
