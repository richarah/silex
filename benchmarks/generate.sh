#!/bin/bash
# Generate static benchmark dashboard HTML from results files
set -e
OUTPUT="$(dirname "$0")/index.html"
RESULTS_DIR="$(dirname "$0")/../../benchmarks/results"

cat > "$OUTPUT" <<'HTMLEOF'
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>Silex Benchmarks</title>
<style>
body{font-family:monospace;max-width:800px;margin:2em auto;padding:0 1em}
table{border-collapse:collapse;width:100%}
td,th{border:1px solid #ccc;padding:0.4em 0.8em;text-align:left}
th{background:#f4f4f4}
</style>
</head>
<body>
<h1>Silex Build Benchmarks</h1>
HTMLEOF

if compgen -G "$RESULTS_DIR"/*.txt > /dev/null 2>&1; then
    for f in "$RESULTS_DIR"/*.txt; do
        echo "<h2>$(basename $f .txt)</h2><pre>" >> "$OUTPUT"
        cat "$f" >> "$OUTPUT"
        echo "</pre>" >> "$OUTPUT"
    done
else
    echo "<p>No benchmark results found. Run <code>benchmarks/benchmark.sh</code> to generate results.</p>" >> "$OUTPUT"
fi

echo "</body></html>" >> "$OUTPUT"
echo "Generated $OUTPUT"
