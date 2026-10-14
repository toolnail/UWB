"""Read all valid per-packet RSSI values; export one arithmetic dBm mean per run.
Source CSVs are read only. The run selection matches the existing success-rate plot.
"""
from pathlib import Path
import hashlib
import io
import json
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager, ticker
import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
ROOT = OUT.parent
MANIFEST = ROOT / 'distance_success_rate_0_200cm/success_rate_vs_distance.csv'


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    runs = pd.read_csv(MANIFEST)
    assert len(runs) == 30 and runs.distance_cm.is_unique
    rows, sources = [], []
    for _, run in runs.iterrows():
        folder = ROOT / run.result_folder
        prefix = f'round_{int(run["round"])}'
        source = folder / f'{prefix}.rx.rssi.csv'
        source_bytes = source.read_bytes()
        source_hash = hashlib.sha256(source_bytes).hexdigest()
        data = pd.read_csv(io.BytesIO(source_bytes), na_values=['NA'],
                           usecols=['sequence', 'reception_id', 'rssi_dbm', 'rssi_valid',
                                    'duplicate', 'rssi_q8'])
        capture = json.loads((folder / f'{prefix}.rx.rssi.capture.json').read_text(encoding='utf-8-sig'))
        assert capture['complete'] and len(data) == capture['rows']
        assert data.reception_id.is_unique and data.sequence.lt(capture['target']).all()
        unique = data.duplicate.eq(0)
        assert int(unique.sum()) == int(run.received_unique) == capture['unique']
        assert int((~unique).sum()) == capture['duplicates']
        assert data.loc[unique, 'sequence'].is_unique
        valid = unique & data.rssi_valid.eq(1) & np.isfinite(data.rssi_dbm)
        values = data.loc[valid]
        assert len(values) > 0
        assert np.allclose(values.rssi_dbm, values.rssi_q8 / 256, atol=1e-8, rtol=0)
        # Average exactly over the saved fixed-point measurements, then convert.
        mean_dbm = int(values.rssi_q8.sum()) / (256 * len(values))
        assert abs(mean_dbm - values.rssi_dbm.mean()) < 1e-8
        row = dict(distance_cm=int(run.distance_cm), round=int(run['round']),
                   received_unique=int(unique.sum()), valid_rssi_samples=len(values),
                   invalid_rssi_samples=int((unique & ~valid).sum()),
                   duplicate_records=int((~unique).sum()), mean_rssi_dbm=mean_dbm,
                   min_rssi_dbm=float(values.rssi_dbm.min()),
                   max_rssi_dbm=float(values.rssi_dbm.max()), result_folder=run.result_folder)
        result = pd.read_csv(folder / 'results.csv')
        result = result.loc[result['round'].eq(run['round'])].iloc[0]
        # Firmware means undergo Q8 truncation plus rounding to two decimals.
        if int(result.signal_samples) == len(values):
            assert abs(mean_dbm - float(result.rssi_avg_dbm)) <= 0.009
        rows.append(row)
        sources.append(dict(file=str(source), sha256=source_hash,
                            csv_rows=len(data), capture_complete=True))

    summary = pd.DataFrame(rows).sort_values('distance_cm').reset_index(drop=True)
    summary.to_csv(OUT / 'mean_rssi_vs_distance.csv', index=False, float_format='%.8f')
    for font in ['times.ttf', 'timesbd.ttf']:
        font_manager.fontManager.addfont(str(Path('C:/Windows/Fonts') / font))
    plt.rcParams.update({
        'font.family': 'serif', 'font.serif': ['Times New Roman'],
        'mathtext.fontset': 'stix', 'font.size': 14.5,
        'axes.labelsize': 15.5, 'axes.labelweight': 'bold', 'axes.linewidth': 1.55,
        'xtick.labelsize': 16, 'ytick.labelsize': 16,
        'xtick.direction': 'out', 'ytick.direction': 'out',
        'xtick.major.size': 5.5, 'ytick.major.size': 5.5,
        'xtick.major.width': 1.45, 'ytick.major.width': 1.45,
        'pdf.fonttype': 42, 'ps.fonttype': 42, 'svg.fonttype': 'none',
        'savefig.transparent': False, 'savefig.facecolor': 'white',
        'axes.unicode_minus': False,
    })
    width_pt, height_pt = 789.3235890909, 600.0
    left_pt, right_pt = 70.0, 0.05 * 194.537
    bottom_pt, top_pt = 0.20 * 190.253, 0.12 * 190.253
    rect = (left_pt / width_pt, bottom_pt / height_pt,
            (width_pt - left_pt - right_pt) / width_pt,
            (height_pt - bottom_pt - top_pt) / height_pt)
    fig = plt.figure(figsize=(width_pt / 72, height_pt / 72), facecolor='white')
    ax = fig.add_axes(rect, facecolor='white')
    ax.set_axisbelow(True)
    ax.grid(True, which='major', color='#D9D9D9', linewidth=0.85, alpha=0.62)
    for spine in ax.spines.values():
        spine.set_color('#3F3F3F')
        spine.set_linewidth(1.55)
    ax.plot(summary.distance_cm, summary.mean_rssi_dbm, color='#0072B2',
            linewidth=3.25, solid_capstyle='round', alpha=1,
            marker='o', markersize=6.5, markerfacecolor='white',
            markeredgecolor='#0072B2', markeredgewidth=1.45, rasterized=False)
    ax.set_xlim(-10, 410)
    ax.set_xticks(np.arange(0, 401, 50))
    lower = np.floor(summary.mean_rssi_dbm.min() / 5) * 5 - 2
    upper = np.ceil(summary.mean_rssi_dbm.max() / 5) * 5 + 2
    ax.set_ylim(lower, upper)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(5))
    ax.set_xlabel('Distance (cm)', labelpad=2)
    ax.set_ylabel('Mean RSSI (dBm)', labelpad=2)
    ax.tick_params(axis='y', pad=0.5)
    ax.tick_params(axis='x', pad=2)
    for label in ax.get_xticklabels() + ax.get_yticklabels():
        label.set_fontfamily('Times New Roman')
        label.set_fontweight('bold')
        label.set_fontsize(16)
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    text_bounds = []
    for label in [ax.xaxis.label, ax.yaxis.label, *ax.get_xticklabels(), *ax.get_yticklabels()]:
        box = label.get_window_extent(renderer).transformed(fig.dpi_scale_trans.inverted())
        bbox = [box.x0 * 72, box.y0 * 72, box.x1 * 72, box.y1 * 72]
        # MultipleLocator may construct invisible tick labels outside the range.
        if label in ax.get_yticklabels() and not lower <= label.get_position()[1] <= upper:
            continue
        assert bbox[0] >= 0 and bbox[1] >= 0 and bbox[2] <= width_pt and bbox[3] <= height_pt
        text_bounds.append(dict(text=label.get_text(), bbox_pt=bbox))
    assert np.array_equal(ax.lines[0].get_ydata(), summary.mean_rssi_dbm.to_numpy())
    output = OUT / 'mean_rssi_vs_distance_ieee.pdf'
    fig.savefig(output, format='pdf', transparent=False, facecolor='white')
    plt.close(fig)
    # Check the source files still match the bytes used for averaging.
    for source in sources:
        assert hashlib.sha256(Path(source['file']).read_bytes()).hexdigest() == source['sha256']
    metadata = dict(run_manifest=str(MANIFEST), points=len(summary),
                    averaging='Arithmetic mean of per-packet RSSI in dBm; sum(rssi_q8)/(256*N)',
                    population='Unique successfully received DATA packets with valid finite RSSI',
                    total_received_unique=int(summary.received_unique.sum()),
                    total_valid_rssi_samples=int(summary.valid_rssi_samples.sum()),
                    total_invalid_rssi_samples=int(summary.invalid_rssi_samples.sum()),
                    distance_unit='cm', y_unit='dBm', y_scale='linear',
                    x_range=[-10, 410], y_range=[float(lower), float(upper)],
                    source_files_unchanged=True, sampling='none', smoothing='none',
                    canvas_pt=[width_pt, height_pt], vector_pdf=True,
                    font='Times New Roman', tick_fontsize_pt=16,
                    labels_bbox_pt=text_bounds, sources=sources)
    (OUT / 'mean_rssi_vs_distance.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(summary[['distance_cm', 'received_unique', 'valid_rssi_samples',
                   'invalid_rssi_samples', 'mean_rssi_dbm']].to_string(index=False))
    print('Total valid RSSI records:',metadata['total_valid_rssi_samples'])


if __name__ == '__main__':
    main()
