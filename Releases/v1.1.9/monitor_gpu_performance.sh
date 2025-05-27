#!/bin/bash

echo "🔍 NDI Plugin GPU Performance Monitor"
echo "======================================"
echo ""

# Check if Metal is available
echo "🖥️ System GPU Information:"
system_profiler SPDisplaysDataType | grep -A 5 "Metal"
echo ""

# Monitor GPU usage in real-time
echo "📊 Starting GPU monitoring..."
echo "Press Ctrl+C to stop monitoring"
echo ""
echo "Timestamp                | GPU Usage | Metal Apps | Memory Usage"
echo "-------------------------|-----------|------------|-------------"

while true; do
    # Get current timestamp
    timestamp=$(date "+%Y-%m-%d %H:%M:%S")
    
    # Get GPU usage (if available)
    gpu_usage=$(sudo powermetrics -n 1 -i 1000 --samplers gpu_power 2>/dev/null | grep "GPU Power" | awk '{print $3}' | head -1)
    if [ -z "$gpu_usage" ]; then
        gpu_usage="N/A"
    fi
    
    # Check for Metal processes
    metal_processes=$(ps aux | grep -i metal | grep -v grep | wc -l)
    
    # Get GPU memory usage (approximate)
    gpu_memory=$(vm_stat | grep "Pages wired down" | awk '{print $4}' | sed 's/\.//')
    if [ -z "$gpu_memory" ]; then
        gpu_memory="N/A"
    else
        gpu_memory="${gpu_memory} pages"
    fi
    
    printf "%-24s | %-9s | %-10s | %s\n" "$timestamp" "$gpu_usage" "$metal_processes" "$gpu_memory"
    
    sleep 2
done 