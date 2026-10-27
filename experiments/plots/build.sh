#!/bin/sh
# Rebuild figure PDFs from the editable SVGs.
# SVG user units are typographic points, so each PDF is exactly \textwidth (14.7cm)
# wide and its text renders at the size given in the SVG.
# Ghostscript re-writes the cairo output to PDF 1.5, which pdfTeX accepts.
set -e
cd "$(dirname "$0")"
for f in "$@"; do
  rsvg-convert -f pdf "$f.svg" -o "_$f.pdf"
  gs -q -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.5 \
     -dPDFSETTINGS=/prepress -dEmbedAllFonts=true \
     -sOutputFile="$f.pdf" "_$f.pdf"
  rm -f "_$f.pdf"
  echo "  $f.pdf"
done
