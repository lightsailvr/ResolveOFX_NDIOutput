# GPU Performance Verification Guide

## 🚀 How to Confirm Metal GPU Acceleration is Working

### 1. Console Log Monitoring (Primary Method)

The plugin now includes detailed logging that will show you exactly when GPU acceleration is being used:

#### **What to Look For:**

**✅ GPU Acceleration Working:**
```
NDI Plugin: Metal GPU acceleration initialized successfully
NDI Plugin: 🚀 Attempting Metal GPU acceleration...
NDI Plugin: Starting Metal GPU RGBA->UYVY conversion (1920x1080)
NDI Plugin: Dispatching 60 x 34 threadgroups with 16 x 16 threads each
NDI Plugin: ✅ Metal GPU RGBA->UYVY conversion completed in 1250 μs (1.25 ms)
NDI Plugin: ✅ Metal GPU acceleration SUCCESS!
```

**❌ CPU Fallback:**
```
NDI Plugin: ⚠️ GPU context not available, falling back to CPU
NDI Plugin: Starting CPU RGBA->UYVY conversion (1920x1080)
NDI Plugin: 🖥️ CPU RGBA->UYVY conversion completed in 8500 μs (8.50 ms)
```

#### **How to View Logs:**

1. **In DaVinci Resolve:**
   - Open Console.app (Applications > Utilities > Console)
   - Filter for "NDI Plugin" in the search box
   - Start playback in DaVinci Resolve with the NDI plugin active

2. **Command Line:**
   ```bash
   # Monitor logs in real-time
   log stream --predicate 'eventMessage CONTAINS "NDI Plugin"' --info
   ```

### 2. Performance Timing Comparison

The logs will show timing differences between GPU and CPU:

- **GPU (Metal):** Typically 1-3ms for 1080p, 3-8ms for 4K
- **CPU:** Typically 5-15ms for 1080p, 20-50ms for 4K

**Expected Performance Improvement:**
- 1080p: 3-5x faster with GPU
- 4K: 5-10x faster with GPU
- HDR: 2-4x faster with GPU

### 3. System GPU Monitoring

#### **Activity Monitor:**
1. Open Activity Monitor
2. Go to "GPU" tab
3. Look for GPU usage spikes when NDI plugin is active

#### **GPU Performance Script:**
```bash
# Run the included monitoring script
./scripts/monitor_gpu_performance.sh
```

### 4. Metal System Verification

#### **Check Metal Support:**
```bash
# Verify Metal 3 support
system_profiler SPDisplaysDataType | grep -A 5 "Metal"
```

#### **Metal Performance HUD (Advanced):**
```bash
# Enable Metal Performance HUD for visual confirmation
export MTL_HUD_ENABLED=1
# Then launch DaVinci Resolve
```

### 5. Plugin Interface Verification

Look for the **"Plugin Version"** field in the NDI Output plugin interface:
- Should show: `v1.1.9 (GPU-Accelerated NDI Advanced)`
- Confirms you have the GPU-enabled version

### 6. Performance Test Scenarios

#### **Test 1: Resolution Scaling**
- Test with different resolutions (1080p, 4K, 8K)
- GPU acceleration benefits increase with resolution
- Monitor timing logs for performance differences

#### **Test 2: HDR vs SDR**
- Enable/disable HDR in plugin settings
- Both should show GPU acceleration logs
- HDR conversion uses different Metal kernel

#### **Test 3: GPU Toggle**
- Disable "GPU Acceleration" in plugin settings
- Should see CPU fallback logs
- Re-enable to confirm GPU acceleration returns

### 7. Troubleshooting GPU Issues

#### **If GPU Acceleration Isn't Working:**

1. **Check Metal Availability:**
   ```bash
   # Should return "Metal 3" or similar
   system_profiler SPDisplaysDataType | grep Metal
   ```

2. **Verify Plugin Version:**
   - Ensure you're using v1.1.9 or later
   - Check version label in plugin interface

3. **Check Console for Errors:**
   ```bash
   log show --last 1h --predicate 'eventMessage CONTAINS "NDI Plugin"' --info
   ```

4. **Common Issues:**
   - **"Metal is not available"**: Older Mac without Metal support
   - **"Failed to create Metal device"**: GPU driver issues
   - **"Metal GPU conversion failed"**: Shader compilation issues

### 8. Performance Benchmarking

#### **Manual Timing Test:**
1. Enable GPU acceleration
2. Note timing from logs (e.g., "1.25 ms")
3. Disable GPU acceleration
4. Note CPU timing from logs (e.g., "8.50 ms")
5. Calculate speedup: CPU_time / GPU_time

#### **Expected Results:**
- **1080p RGBA→UYVY:** 3-5x speedup
- **4K RGBA→UYVY:** 5-10x speedup
- **HDR 16-bit conversion:** 2-4x speedup

### 9. Visual Confirmation Methods

#### **Frame Rate Monitoring:**
- Higher frame rates with GPU acceleration
- Smoother playback in DaVinci Resolve
- Reduced CPU usage in Activity Monitor

#### **Thermal Monitoring:**
- GPU temperature increase (normal)
- CPU temperature decrease (less CPU load)
- Overall system runs cooler

### 10. Advanced Verification

#### **Metal Debugger (Xcode Required):**
1. Install Xcode
2. Open Instruments
3. Select "Metal System Trace"
4. Profile DaVinci Resolve with NDI plugin active
5. Look for Metal command buffer submissions

#### **GPU Memory Usage:**
```bash
# Monitor GPU memory allocation
sudo powermetrics -n 1 -i 1000 --samplers gpu_power
```

---

## 🎯 Quick Verification Checklist

- [ ] Plugin version shows "GPU-Accelerated NDI Advanced"
- [ ] Console logs show "Metal GPU acceleration initialized successfully"
- [ ] Conversion logs show "✅ Metal GPU acceleration SUCCESS!"
- [ ] Timing shows <5ms for 1080p conversion
- [ ] Activity Monitor shows GPU usage spikes
- [ ] System has Metal 3 support
- [ ] No error messages in console logs

If all items are checked, your GPU acceleration is working perfectly! 🚀 