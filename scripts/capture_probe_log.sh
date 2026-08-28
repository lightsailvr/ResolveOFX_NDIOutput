#!/bin/bash
# Capture render-probe log lines to a file for the findings report.
#
# Usage:  ./scripts/capture_probe_log.sh <scenario-slug>
# Writes: docs/captures/YYYY-MM-DD-<scenario-slug>.log   (Ctrl-C to stop)
#
# Prereqs: plugin >= 1.3.0 installed, and "Log Render Calls" enabled in the
# plugin's Diagnostics parameter group. Scenario procedures live in
# docs/2026-08-28-render-call-probe-findings.md.
set -euo pipefail

SLUG="${1:?usage: $0 <scenario-slug>   e.g. $0 stereo-timeline-edit-page}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$REPO_ROOT/docs/captures"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/$(date +%F)-$SLUG.log"
# Never overwrite an earlier run of the same scenario: suffix -2, -3, …
N=2
while [ -e "$OUT" ]; do
    OUT="$OUT_DIR/$(date +%F)-$SLUG-$N.log"
    N=$((N + 1))
done

echo "Capturing 'NDI Plugin: probe' lines to:"
echo "  $OUT"
echo "Reminder: 'Log Render Calls' must be ON in the plugin's Diagnostics group."
echo "Start playback in Resolve now; Ctrl-C here when the scenario is done."
echo ""

# Absolute path: interactive shells on this machine shadow `log` with a function.
/usr/bin/log stream --predicate 'eventMessage CONTAINS "NDI Plugin: probe"' --info --style compact | tee "$OUT"
