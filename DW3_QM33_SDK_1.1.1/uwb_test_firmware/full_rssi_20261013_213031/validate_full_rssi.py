"""Independent audit of CSV rows against the firmware summary and old sampled
text log. This script is for the archived real-board validation run."""
import csv
import json
from pathlib import Path
import re

directory = Path(__file__).resolve().parent
run = directory.parent / 'repeat_20261013_213324_051'
lines = (run / 'round_1.rx.log').read_text(encoding='utf-8-sig').splitlines()
def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))
complete = fields(next(x for x in lines if x.startswith('RX_COMPLETE ')))
summary = fields(next(x for x in lines if x.startswith('RX_SIGNAL_COMPLETE ')))
sampled = {(int(v['seq']), int(v['reception_id'])): v for v in
           (fields(x) for x in lines if x.startswith('RX_SIGNAL seq='))}

def format_q8(value):
    cents = (abs(value)*100+128)//256
    return f'{"-" if value < 0 else ""}{cents//100}.{cents%100:02d}'

def mean_q8(total, count):
    return (1 if total >= 0 else -1) * (abs(total)//count)

rows = unique = duplicates = valid = 0
sums = [0, 0]
minima = [32767, 32767]
maxima = [-32768, -32768]
matched = 0
with open(run / 'round_1.rx.rssi.csv', encoding='utf-8-sig', newline='') as source:
    for row in csv.DictReader(source):
        rows += 1
        duplicate = int(row['duplicate'])
        duplicates += duplicate
        unique += 1 - duplicate
        key = (int(row['sequence']), int(row['reception_id']))
        rssi, fp = int(row['rssi_q8']), int(row['fp_power_q8'])
        if key in sampled:
            old = sampled[key]
            assert format_q8(rssi) == old['rssi_dbm']
            assert format_q8(fp) == old['fp_power_dbm']
            matched += 1
        if not duplicate and int(row['diag_valid']):
            valid += 1
            for index, value in enumerate((rssi, fp)):
                sums[index] += value
                minima[index] = min(minima[index], value)
                maxima[index] = max(maxima[index], value)
assert unique == int(complete['success'])
assert duplicates == int(complete['dup'])
assert valid == int(summary['samples'])
assert unique - valid == int(summary['invalid'])
assert matched == len(sampled)
for index, prefix in enumerate(('rssi', 'fp')):
    assert format_q8(mean_q8(sums[index], valid)) == summary[f'{prefix}_avg_dbm']
    assert format_q8(minima[index]) == summary[f'{prefix}_min_dbm']
    assert format_q8(maxima[index]) == summary[f'{prefix}_max_dbm']
capture = json.loads((run / 'round_1.rx.rssi.capture.json').read_text())
assert capture['complete'] and capture['rows'] == rows and capture['stream_dropped'] == 0
result = {'passed': True, 'csv_rows': rows, 'unique': unique, 'duplicates': duplicates,
          'valid_rssi_fp': valid, 'sampled_text_rows_matched': matched,
          'aggregate_statistics_match': True, 'stream_dropped': 0}
(directory / 'validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
