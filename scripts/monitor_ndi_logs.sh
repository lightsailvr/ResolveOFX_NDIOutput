#!/bin/bash

echo "🔍 NDI Plugin macOS System Log Monitor"
echo "======================================"
echo ""
echo "This script monitors macOS system logs for NDI Plugin messages."
echo "The plugin now uses os_log() which integrates with macOS Console."
echo ""
echo "Instructions:"
echo "1. Start DaVinci Resolve"
echo "2. Add NDI Output plugin to your timeline"
echo "3. Start playback to trigger frame processing"
echo "4. Watch for NDI Plugin messages below"
echo ""
echo "Press Ctrl+C to stop monitoring"
echo ""

# Monitor logs in real-time for NDI Plugin messages
echo "📊 Real-time log monitoring (waiting for NDI Plugin messages)..."
echo "Looking for messages containing 'NDI Plugin' or 'NDI Plugin Metal'..."
echo ""

# Use log stream to monitor in real-time
log stream --predicate 'eventMessage CONTAINS "NDI Plugin"' --info --style compact 