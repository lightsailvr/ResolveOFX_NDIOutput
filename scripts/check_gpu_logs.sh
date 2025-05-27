#!/bin/bash

echo "🔍 NDI Plugin GPU Acceleration Log Monitor"
echo "=========================================="
echo ""
echo "Instructions:"
echo "1. Start DaVinci Resolve"
echo "2. Add NDI Output plugin to your timeline"
echo "3. Start playback to trigger frame processing"
echo "4. Watch for GPU acceleration messages below"
echo ""
echo "Press Ctrl+C to stop monitoring"
echo ""

# Monitor logs in real-time
echo "📊 Real-time log monitoring (waiting for NDI Plugin messages)..."
log stream --predicate 'eventMessage CONTAINS "NDI Plugin"' --info --style compact

# Alternative: Check recent logs if real-time doesn't work
echo ""
echo "📋 Recent logs from last 10 minutes:"
log show --last 10m --predicate 'eventMessage CONTAINS "NDI Plugin"' --info --style compact 