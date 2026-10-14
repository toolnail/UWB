"""One vector RSSI/sequence plot per available distance test, with all valid points.
Reads source files without editing them; records source hashes and incomplete folders.
"""
from collections import Counter
from pathlib import Path
import hashlib
import io
import json
import re
import sys
import zipfile

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager, ticker
import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
ROOT = OUT.parent


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    jobs, skipped = [], []
    for folder in ROOT.glob('repeat_dis*'):
        match = re.fullmatch(r'repeat_dis(\d+)_(\d{8}_\d{6}_\d{3})', folder.name)
        if not match:
            continue
        files = sorted(folder.glob('round_*.rx.rssi.csv'))
        if not files:
            skipped.append(dict(folder=folder.name, distance_cm=int(match[1]),
                                reason='No per-packet RSSI CSV exists'))
            continue
        for source in files:
            round_match = re.fullmatch(r'round_(\d+)\.rx\.rssi\.csv', source.name)
            if not round_match:
                continue
            capture_path = source.with_name(source.name.replace('.csv', '.capture.json'))
            if not capture_path.is_file():
                skipped.append(dict(folder=folder.name, distance_cm=int(match[1]),
                                    reason='No capture-completion metadata', file=source.name))
                continue
            capture = json.loads(capture_path.read_text(encoding='utf-8-sig'))
            if not capture.get('complete'):
                skipped.append(dict(folder=folder.name, distance_cm=int(match[1]),
                                    reason='Capture is incomplete', file=source.name))
                continue
            jobs.append(dict(distance_cm=int(match[1]), timestamp=match[2],
                             round=int(round_match[1]), source=source, capture=capture))
    jobs.sort(key=lambda j: (j['distance_cm'], j['timestamp'], j['round']))
    distance_counts = Counter(j['distance_cm'] for j in jobs)
    assert jobs, 'No completed RSSI captures available'
    print(f'Available captures: {len(jobs)}; folders/captures skipped: {len(skipped)}', flush=True)

    for name in ['times.ttf', 'timesbd.ttf']:
        font_manager.fontManager.addfont(str(Path('C:/Windows/Fonts') / name))
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
    index, evidence = [], []
    for number, job in enumerate(jobs, 1):
        source, capture = job['source'], job['capture']
        raw_bytes = source.read_bytes()
        digest = hashlib.sha256(raw_bytes).hexdigest()
        data = pd.read_csv(io.BytesIO(raw_bytes), na_values=['NA'],
                           usecols=['sequence', 'reception_id', 'rssi_dbm',
                                    'rssi_valid', 'rssi_q8', 'duplicate'])
        assert len(data) == capture['rows'] and data.reception_id.is_unique
        assert data.sequence.between(0, capture['target'] - 1).all()
        assert int(data.duplicate.eq(0).sum()) == capture['unique']
        assert int(data.duplicate.eq(1).sum()) == capture['duplicates']
        valid = data.rssi_valid.eq(1) & np.isfinite(data.rssi_dbm)
        plotted = data.loc[valid]
        assert len(plotted) > 0
        assert np.allclose(plotted.rssi_dbm, plotted.rssi_q8 / 256, atol=1e-8, rtol=0)
        suffix = '' if distance_counts[job['distance_cm']] == 1 else f'_{job["timestamp"]}_round{job["round"]}'
        name = f'rssi_dis{job["distance_cm"]}cm{suffix}.pdf'
        output = OUT / name
        width_pt, height_pt = 600.0, 300.0
        fig = plt.figure(figsize=(width_pt / 72, height_pt / 72), facecolor='white')
        ax = fig.add_axes((70 / width_pt, 55 / height_pt,
                          495 / width_pt, 225 / height_pt), facecolor='white')
        ax.set_axisbelow(True)
        ax.grid(True, which='major', color='#D9D9D9', linewidth=0.85, alpha=0.62)
        for spine in ax.spines.values():
            spine.set_color('#3F3F3F')
            spine.set_linewidth(1.55)
        # Scatter every packet. Connecting the alternating levels would obscure
        # the distribution; the PDF keeps each point as a vector mark.
        points = ax.scatter(plotted.sequence, plotted.rssi_dbm, s=0.35, marker='.',
                            color='#0072B2', alpha=1, linewidths=0, rasterized=False)
        assert len(points.get_offsets()) == len(plotted)
        target = capture['target']
        ax.set_xlim(-0.015 * target, 1.015 * target)
        ax.set_xticks(np.linspace(0, target, 7))
        ax.xaxis.set_major_formatter(ticker.StrMethodFormatter('{x:,.0f}'))
        lower = np.floor(plotted.rssi_dbm.min() / 5) * 5 - 1
        upper = np.ceil(plotted.rssi_dbm.max() / 5) * 5 + 1
        ax.set_ylim(lower, upper)
        ax.yaxis.set_major_locator(ticker.MaxNLocator(nbins=7, steps=[1, 2, 2.5, 5, 10]))
        ax.set_xlabel('Packet number (sequence)', labelpad=3)
        ax.set_ylabel('RSSI (dBm)', labelpad=3)
        ax.tick_params(axis='y', pad=1)
        ax.tick_params(axis='x', pad=2)
        for label in ax.get_xticklabels() + ax.get_yticklabels():
            label.set_fontfamily('Times New Roman')
            label.set_fontweight('bold')
            label.set_fontsize(16)
        fig.canvas.draw()
        renderer = fig.canvas.get_renderer()
        for label in [ax.xaxis.label, ax.yaxis.label, *ax.get_xticklabels(), *ax.get_yticklabels()]:
            if label in ax.get_yticklabels() and not lower <= label.get_position()[1] <= upper:
                continue
            box = label.get_window_extent(renderer).transformed(fig.dpi_scale_trans.inverted())
            assert box.x0 >= 0 and box.y0 >= 0 and box.x1 * 72 <= width_pt and box.y1 * 72 <= height_pt, (label.get_text(), box.bounds)
        fig.savefig(output, format='pdf', transparent=False, facecolor='white')
        plt.close(fig)
        assert b'/Subtype /Image' not in output.read_bytes(), 'Unexpected raster image in PDF'
        row = dict(distance_cm=job['distance_cm'], round=job['round'],
                   received_records=len(data), plotted_valid_rssi_records=len(plotted),
                   invalid_rssi_records=int((~valid).sum()), duplicate_records=int(data.duplicate.sum()),
                   sequence_min=int(plotted.sequence.min()), sequence_max=int(plotted.sequence.max()),
                   mean_rssi_dbm=float(plotted.rssi_dbm.mean()), min_rssi_dbm=float(plotted.rssi_dbm.min()),
                   max_rssi_dbm=float(plotted.rssi_dbm.max()), y_axis_min_dbm=float(lower),
                   y_axis_max_dbm=float(upper), result_folder=source.parent.name,
                   source_csv=source.name, pdf_file=name)
        index.append(row)
        evidence.append(dict(source=str(source), source_sha256=digest, output=str(output),
                             vector_pdf=True, plotted_rows=len(plotted)))
        print(f'[{number}/{len(jobs)}] dis{job["distance_cm"]}cm: {len(plotted):,} valid points -> {name}', flush=True)

    for source in evidence:
        assert hashlib.sha256(Path(source['source']).read_bytes()).hexdigest() == source['source_sha256']
    frame = pd.DataFrame(index)
    frame.to_csv(OUT / 'plot_index.csv', index=False, float_format='%.8f')
    manifest = dict(charts=len(index), total_plotted_rows=int(frame.plotted_valid_rssi_records.sum()),
                    x_field='sequence: transmitted DATA packet number',
                    y_field='rssi_dbm: per-reception RSSI, not an averaged value',
                    sampling='none', smoothing='none', missing_packets='not filled',
                    y_limits='Each chart includes its full valid RSSI range; axes are independently scaled',
                    original_source_files_unchanged=True, sources=evidence, skipped_folders=skipped)
    (OUT / 'plot_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    with zipfile.ZipFile(OUT / 'rssi_vs_packet_number_all_distances.zip', 'w', compression=zipfile.ZIP_DEFLATED) as bundle:
        for row in index:
            bundle.write(OUT / row['pdf_file'], arcname=row['pdf_file'])
        for extra in ['plot_index.csv', 'plot_manifest.json']:
            bundle.write(OUT / extra, arcname=extra)
    print(json.dumps({'charts': len(index), 'points': manifest['total_plotted_rows'],
                      'skipped_folders': len(skipped)}, indent=2), flush=True)


if __name__ == '__main__':
    main()
