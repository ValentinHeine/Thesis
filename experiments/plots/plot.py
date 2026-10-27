#!/usr/bin/env python3
"""Regenerate the evaluation figures from the CSV files in ../data.

The figures are drawn directly as SVG so that one user unit is one typographic
point: the resulting PDF is exactly \textwidth wide and its labels come out at
the size given here, in the same Latin Modern faces as the body text.
Series are separated by dash pattern and marker shape as well as by grey level,
so the plots survive black-and-white printing.
"""
import csv, os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

W = 417.0
PRE = """<svg xmlns="http://www.w3.org/2000/svg" width="{w}pt" height="{h}pt" viewBox="0 0 {w} {h}">
  <style>
    text {{ font-family: "Latin Modern Roman", "Liberation Serif", serif; font-size: 8px; fill: #000; }}
    .c {{ text-anchor: middle; }}
    .e {{ text-anchor: end; }}
    line, path, polyline {{ fill: none; stroke: #000; stroke-width: 0.6; }}
    .g {{ stroke: #b0b0b0; stroke-width: 0.4; }}
  </style>
"""


def read(name):
    with open(os.path.join(DATA, name)) as fh:
        return list(csv.DictReader(fh))


def fmt_k(v):
    if v == 0:
        return "0"
    return "%g k" % (v / 1e3)


def marker(shape, x, y, fill):
    r = 1.9
    if shape == "o":
        return '<circle cx="%.2f" cy="%.2f" r="%.2f" fill="%s" stroke="#000" stroke-width="0.5"/>' % (x, y, r, fill)
    if shape == "s":
        return '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s" stroke="#000" stroke-width="0.5"/>' % (
            x - r, y - r, 2 * r, 2 * r, fill)
    d = r * 1.25
    return '<path d="M%.2f,%.2f L%.2f,%.2f L%.2f,%.2f Z" fill="%s" stroke="#000" stroke-width="0.5"/>' % (
        x, y - d, x + d, y + d * 0.75, x - d, y + d * 0.75, fill)


def lineplot(out, height, cats, series, xlabel, ylabel, ymax, ystep, legend_at):
    """series: list of (label, {cat: value}, dash, shape, fill)"""
    L, R, T, B = 44.0, 5.0, 7.0, 30.0
    x0, x1 = L, W - R
    y0, y1 = T, height - B
    step = (x1 - x0) / len(cats)

    def px(i):
        return x0 + step * (i + 0.5)

    def py(v):
        return y1 - (v / ymax) * (y1 - y0)

    s = [PRE.format(w=W, h=height)]
    # horizontal rules at the ticks, behind everything
    v = 0
    while v <= ymax + 1:
        s.append('<line class="g" x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x0, py(v), x1, py(v)))
        s.append('<text x="%.2f" y="%.2f" class="e">%s</text>' % (x0 - 4, py(v) + 2.8, fmt_k(v)))
        v += ystep
    # axes
    s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x0, y0, x0, y1))
    s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x0, y1, x1, y1))
    for i, c in enumerate(cats):
        s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (px(i), y1, px(i), y1 + 2.5))
        s.append('<text x="%.2f" y="%.2f" class="c">%s</text>' % (px(i), y1 + 11, c))
    s.append('<text x="%.2f" y="%.2f" class="c">%s</text>' % ((x0 + x1) / 2, height - 6, xlabel))
    s.append('<text transform="translate(%.2f,%.2f) rotate(-90)" class="c">%s</text>'
             % (11, (y0 + y1) / 2, ylabel))
    # series; a series measured at a single configuration is extended across the
    # plot as a reference line, with the marker left at the point actually measured
    for label, vals, dash, shape, fill in series:
        pts = [(px(i), py(vals[c])) for i, c in enumerate(cats) if c in vals]
        if len(pts) > 1:
            s.append('<polyline points="%s" stroke-dasharray="%s"/>'
                     % (" ".join("%.2f,%.2f" % p for p in pts), dash))
        elif pts:
            s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke-dasharray="%s"/>'
                     % (x0, pts[0][1], x1, pts[0][1], dash))
        for p in pts:
            s.append(marker(shape, p[0], p[1], fill))
    # legend
    lx, ly = legend_at
    rowh = 10.5
    wid = 6 + 20 + 3 + max(len(l) for l, _, _, _, _ in series) * 3.75
    s.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="#fff" stroke="#000" stroke-width="0.4"/>'
             % (lx, ly, wid, rowh * len(series) + 5))
    for i, (label, _, dash, shape, fill) in enumerate(series):
        cy = ly + 5 + rowh * i + rowh / 2 - 1
        s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke-dasharray="%s"/>'
                 % (lx + 5, cy, lx + 23, cy, dash))
        s.append(marker(shape, lx + 14, cy, fill))
        s.append('<text x="%.2f" y="%.2f">%s</text>' % (lx + 27, cy + 2.8, label))
    s.append("</svg>\n")
    with open(os.path.join(HERE, out), "w") as fh:
        fh.write("\n".join(s))
    print("  " + out)


def workers():
    rows = read("evaluation_workers.csv")
    by = {}
    for r in rows:
        by.setdefault(r["system"], {})[r["threads"]] = float(r["tuples_per_s"])
    cats = ["1", "2", "4", "8", "16"]
    series = [
        ("NebulaStream, CSV control", by["nes_csv"], "none", "s", "#fff"),
        ("NebulaStream, XML", by["nes_xml"], "3 2", "o", "#000"),
        ("Expat SAX, one stream (sequential)", by["expat_sax"], "5 2 1.5 2", "t", "#888"),
    ]
    lineplot("evaluation_workers.svg", 196, cats, series,
             "Worker threads", "Throughput (tuples/s)", 1000000, 200000, (232, 100))


def sources():
    rows = read("evaluation_sources.csv")
    by = {}
    for r in rows:
        by.setdefault(r["system"], {})[r["sources"]] = float(r["tuples_per_s"])
    cats = ["1", "2", "4", "8", "16", "32"]
    series = [
        ("NebulaStream, XML, fixed 8-worker pool", by["nes_xml"], "3 2", "o", "#000"),
        ("Expat SAX, one parser thread per source", by["expat_sax"], "5 2 1.5 2", "t", "#888"),
    ]
    lineplot("evaluation_sources.svg", 196, cats, series,
             "Physical input sources", "Throughput (tuples/s)", 1200000, 200000, (60, 14))


def simd():
    rows = read("simd_prototype.csv")
    height = 120.0
    L, R, T, B = 118.0, 8.0, 8.0, 30.0
    x0, x1 = L, W - R
    y1 = height - B
    xmax = 2000.0
    s = [PRE.format(w=W, h=height)]
    for v in range(0, int(xmax) + 1, 400):
        x = x0 + (v / xmax) * (x1 - x0)
        s.append('<line class="g" x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x, T, x, y1))
        s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x, y1, x, y1 + 2.5))
        s.append('<text x="%.2f" y="%.2f" class="c">%d</text>' % (x, y1 + 11, v))
    s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x0, T, x0, y1))
    s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (x0, y1, x1, y1))
    s.append('<text x="%.2f" y="%.2f" class="c">Indexing throughput (MB/s)</text>'
             % ((x0 + x1) / 2, height - 6))
    names = {"rapidxml": "RapidXML (implemented)",
             "simd": "structural scan",
             "simd_fastnames": "scan + name dispatch"}
    shades = {"rapidxml": "#ffffff", "simd": "#cccccc", "simd_fastnames": "#888888"}
    bh, gap = 16.0, 8.0
    for i, r in enumerate(rows):
        y = T + 4 + i * (bh + gap)
        v = float(r["throughput_mb_s"])
        wbar = (v / xmax) * (x1 - x0)
        s.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s" stroke="#000" stroke-width="0.6"/>'
                 % (x0, y, wbar, bh, shades[r["variant"]]))
        s.append('<text x="%.2f" y="%.2f" class="e">%s</text>' % (x0 - 5, y + bh / 2 + 2.8, names[r["variant"]]))
        s.append('<text x="%.2f" y="%.2f">%d  (%.2f&#215;)</text>'
                 % (x0 + wbar + 4, y + bh / 2 + 2.8, int(v), float(r["speedup"])))
    s.append("</svg>\n")
    with open(os.path.join(HERE, "simd_prototype.svg"), "w") as fh:
        fh.write("\n".join(s))
    print("  simd_prototype.svg")


if __name__ == "__main__":
    workers(); sources(); simd()
