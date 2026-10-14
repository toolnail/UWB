"""Plot final unique-packet delivery rates for the user-selected distance sweep."""
from pathlib import Path
import hashlib
import json
import re
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager, ticker
import numpy as np
import pandas as pd

START = 'repeat_dis0_20261013_220654_694'
LEGACY_END = 'repeat_dis200_20261014_112329_153'
END = 'repeat_dis400_20261014_133732_356'
OUT = Path(__file__).resolve().parent
ROOT = OUT.parent


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    records, sources, excluded_folders = [], [], []
    start_time = START.split('_', 2)[2]
    end_time = END.split('_', 2)[2]
    for folder in ROOT.glob('repeat_dis*'):
        match = re.fullmatch(r'repeat_dis(\d+)_(\d{8}_\d{6}_\d{3})', folder.name)
        if not match:
            continue
        distance, stamp = int(match[1]), match[2]
        if not (0 <= distance <= 400 and start_time <= stamp <= end_time):
            continue
        if distance <= 200 and stamp > LEGACY_END.split('_', 2)[2]:
            continue
        if distance > 200 and distance not in [250, 300, 400]:
            continue
        source = folder / 'results.csv'
        if not source.is_file():
            excluded_folders.append(dict(folder=folder.name, reason='No final results.csv'))
            continue
        for _, row in pd.read_csv(source).iterrows():
            target, received = int(row.target_packets), int(row.received_unique)
            failed, round_id = int(row.failed_packets), int(row['round'])
            assert 0 <= received <= target and received + failed == target
            rate = 100.0 * received / target
            assert abs(rate - float(row.success_percent)) < 1e-9
            log_path = folder / f'round_{round_id}.rx.log'
            log = log_path.read_text(encoding='utf-8-sig')
            complete = [line for line in log.splitlines() if line.startswith('RX_COMPLETE ')]
            assert len(complete) == 1
            values = dict(re.findall(r'(\w+)=(\S+)', complete[0]))
            assert int(values['target']) == target
            assert int(values['success']) == received
            assert int(values['failed']) == failed
            assert int(values['end_seq']) == target
            capture = json.loads((folder / f'round_{round_id}.rx.rssi.capture.json').read_text(encoding='utf-8-sig'))
            assert capture['complete'] and capture['target'] == target and capture['unique'] == received
            records.append(dict(distance_cm=distance, round=round_id,
                                target_packets=target, received_unique=received,
                                failed_packets=failed, success_percent=rate,
                                result_folder=folder.name))
        sources.append(dict(folder=folder.name,
                            results_sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
    data = pd.DataFrame(records).sort_values(['distance_cm', 'round']).reset_index(drop=True)
    assert data.distance_cm.is_unique, 'Repeated distances require an explicit aggregation choice'
    assert data.iloc[0].result_folder == START and data.iloc[-1].result_folder == END
    assert data.target_packets.eq(300000).all()
    assert data[data.distance_cm <= 200].shape[0] == 27
    assert data[data.distance_cm > 200].distance_cm.tolist() == [250, 300, 400]
    data.to_csv(OUT / 'success_rate_vs_distance.csv', index=False, float_format='%.12f')

    for font in ['times.ttf', 'timesbd.ttf']:
        font_manager.fontManager.addfont(str(Path('C:/Windows/Fonts') / font))
    plt.rcParams.update({
        'font.family': 'serif',
        'font.serif': ['Times New Roman'],
        'mathtext.fontset': 'stix',
        'font.size': 14.5,
        'axes.labelsize': 15.5,
        'axes.labelweight': 'bold',
        'axes.linewidth': 1.55,
        'xtick.labelsize': 16.0,
        'ytick.labelsize': 16.0,
        'xtick.direction': 'out',
        'ytick.direction': 'out',
        'xtick.major.size': 5.5,
        'ytick.major.size': 5.5,
        'xtick.major.width': 1.45,
        'ytick.major.width': 1.45,
        'lines.linewidth': 3.2,
        'pdf.fonttype': 42,
        'ps.fonttype': 42,
        'svg.fonttype': 'none',
        'savefig.transparent': False,
        'savefig.facecolor': 'white',
    })
    canvas_height_pt = 600.0
    # Lengthen the linear vertical axis to separate the precise rate labels,
    # preserving physical font sizes and horizontal point spacing.
    original_width_pt = 194.537
    original_left_pt = 0.26 * original_width_pt
    left_pt, right_pt = 70.0, 0.05 * original_width_pt
    x_range = [-10, int(data.distance_cm.max()) + 10]
    x_span = x_range[1] - x_range[0]
    # Preserve the previous physical spacing between points 5 cm apart.
    canvas_width_pt = left_pt + right_pt + (432.0 - original_left_pt - right_pt) * x_span / 220
    bottom_pt, top_pt = 0.20 * 190.253, 0.12 * 190.253
    axes_fraction = (left_pt / canvas_width_pt, bottom_pt / canvas_height_pt,
                     (canvas_width_pt - left_pt - right_pt) / canvas_width_pt,
                     (canvas_height_pt - bottom_pt - top_pt) / canvas_height_pt)
    fig = plt.figure(figsize=(canvas_width_pt / 72, canvas_height_pt / 72), facecolor='white')
    ax = fig.add_axes(axes_fraction, facecolor='white')
    ax.grid(True, which='major', color='#D9D9D9', linewidth=0.85, alpha=0.62)
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_color('#3F3F3F')
        spine.set_linewidth(1.55)
    ax.plot(data.distance_cm, data.success_percent, color='#0072B2',
            linewidth=3.25, solid_capstyle='round', alpha=1, marker='o',
            markersize=6.5, markerfacecolor='white', markeredgecolor='#0072B2',
            markeredgewidth=1.45,
            rasterized=False, zorder=3)
    ax.set_xlim(*x_range)
    ax.set_xticks(np.arange(0, int(data.distance_cm.max()) + 1, 50))
    y_range = [99.78, 100.01]
    rate_ticks = [99.8, 99.85, 99.9, 99.95, 99.98, 99.99, 99.999]
    ax.set_ylim(*y_range)
    ax.set_yscale('linear')
    ax.set_yticks(rate_ticks)
    ax.yaxis.set_minor_locator(ticker.FixedLocator([99.995, 100.0]))
    ax.yaxis.set_minor_formatter(ticker.NullFormatter())
    ax.tick_params(axis='y', which='minor', length=3, width=1.0)
    ax.grid(False, which='minor')
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda value, pos: f'{value:g}'))
    ax.set_xlabel('Distance (cm)', labelpad=2)
    ax.set_ylabel('Success rate (%)', labelpad=2)
    ax.tick_params(axis='y', pad=0.5)
    ax.tick_params(axis='x', pad=2)
    for label in ax.get_xticklabels() + ax.get_yticklabels():
        label.set_fontfamily('Times New Roman')
        label.set_fontweight('bold')
        label.set_fontsize(16.0)
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    bounds = []
    for label in [ax.xaxis.label, ax.yaxis.label, *ax.get_xticklabels(), *ax.get_yticklabels()]:
        bbox = label.get_window_extent(renderer).transformed(fig.dpi_scale_trans.inverted())
        points = [bbox.x0 * 72, bbox.y0 * 72, bbox.x1 * 72, bbox.y1 * 72]
        bounds.append({'text': label.get_text(), 'bbox_pt': points})
        assert points[0] >= 0 and points[1] >= 0, (label.get_text(), points)
        assert points[2] <= canvas_width_pt and points[3] <= canvas_height_pt, (label.get_text(), points)
    assert np.array_equal(ax.lines[0].get_xdata(), data.distance_cm.to_numpy())
    assert np.array_equal(ax.lines[0].get_ydata(), data.success_percent.to_numpy())
    assert ax.get_yscale() == 'linear'
    y_label_bounds = [label.get_window_extent(renderer) for label in ax.get_yticklabels()]
    for lower, upper in zip(y_label_bounds[:-1], y_label_bounds[1:]):
        assert lower.y1 < upper.y0, 'Vertical tick labels overlap'
    axes_width_pt = canvas_width_pt - left_pt - right_pt
    min_marker_spacing_pt = float(np.diff(data.distance_cm).min()) / x_span * axes_width_pt
    assert min_marker_spacing_pt > 6.5 + 1.45
    output = OUT / 'success_rate_vs_distance_ieee.pdf'
    fig.savefig(output, format='pdf', transparent=False, facecolor='white')
    plt.close(fig)
    metadata = dict(start_folder=START, legacy_end_folder=LEGACY_END, end_folder=END, distance_unit='cm',
                    points=len(data), rounds_per_distance=1,
                    metric='received_unique / target_packets * 100',
                    x_range=x_range, y_range=y_range,
                    y_scale='linear', success_rate_ticks=rate_ticks,
                    unlabeled_minor_rate_ticks=[99.995, 100.0],
                    canvas_pt=[canvas_width_pt, canvas_height_pt], axes_fraction=list(axes_fraction),
                    marker='o', marker_size_pt=6.5, marker_edgewidth_pt=1.45,
                    marker_facecolor='white', min_marker_spacing_pt=min_marker_spacing_pt,
                    font='Times New Roman', base_fontsize_pt=14.5, label_fontsize_pt=15.5,
                    tick_fontsize_pt=16.0,
                    vector_pdf=True, labels_bbox_pt=bounds,
                    interpolation=False, confidence_intervals=None,
                    sources=sorted(sources, key=lambda x: x['folder']),
                    excluded_folders=sorted(excluded_folders, key=lambda x: x['folder']))
    (OUT / 'success_rate_vs_distance.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(data[['distance_cm', 'received_unique', 'failed_packets', 'success_percent']].to_string(index=False))


if __name__ == '__main__':
    main()
