from pathlib import Path
import json
import sys
import pandas as pd
import pypdfium2 as pdfium

root = Path('uwb_test_firmware/distance_packet_rssi_plots')
preview = Path('tmp/pdfs/distance_packet_rssi')
preview.mkdir(parents=True, exist_ok=True)
shard = int(sys.argv[1])
index = pd.read_csv(root / 'plot_index.csv')
verified = []
for number, row in index.iterrows():
    if number % 3 != shard:
        continue
    source = root / row.pdf_file
    binary = source.read_bytes()
    assert b'/Subtype /Image' not in binary
    assert b'TimesNewRomanPS-BoldMT' in binary and b'/FontFile2' in binary
    pdf = pdfium.PdfDocument(str(source))
    assert len(pdf) == 1
    page = pdf[0]
    assert page.get_size() == (600.0, 300.0)
    textpage = page.get_textpage()
    text = textpage.get_text_range()
    assert 'RSSI' in text and 'Packet number' in text
    for char in range(textpage.count_chars()):
        left, bottom, right, top = textpage.get_charbox(char)
        assert left >= -0.01 and bottom >= -0.01 and right <= 600.01 and top <= 300.01
    bitmap = page.render(scale=1.1)
    bitmap.to_pil().save(preview / f'dis{int(row.distance_cm)}cm.png')
    verified.append(dict(distance_cm=int(row.distance_cm), file=row.pdf_file,
                         rendered=True, vector=True, font_embedded=True,
                         no_clipped_text=True, points=int(row.plotted_valid_rssi_records)))
    bitmap.close()
    textpage.close()
    page.close()
    pdf.close()
    print(f'Checked dis{int(row.distance_cm)}cm', flush=True)
(preview / f'verified_shard_{shard}.json').write_text(json.dumps(verified, indent=2), encoding='utf-8')
