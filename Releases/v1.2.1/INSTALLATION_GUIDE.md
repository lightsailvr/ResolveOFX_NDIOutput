# NDI Output Plugin v1.2.1 - Installation Guide

## 🚀 Quick Installation

### Automatic Installation (Recommended)
The plugin has been automatically installed to:
```
/Library/OFX Plugins/NDIOutput.ofx.bundle
```

### Manual Installation
If you need to install manually:

1. **Copy the plugin bundle:**
   ```bash
   sudo cp -r NDIOutput.ofx.bundle "/Library/OFX Plugins/"
   ```

2. **Restart DaVinci Resolve** to load the new plugin

## 🔍 Verifying Installation

### In DaVinci Resolve:
1. Open DaVinci Resolve
2. Go to **Effects Library** > **OpenFX** > **LSVR**
3. Look for **"NDIOutput"** plugin
4. Drag it to your timeline

### Check Version:
1. Select the NDI Output effect on your timeline
2. In the **Inspector** panel, look for:
   - **"Plugin Information"** group at the top
   - **"Plugin Version"** field showing: `v1.2.1 (GPU-Accelerated NDI Advanced)`

## 📋 Interface Layout

The plugin interface is now organized into groups:

### 🔹 Plugin Information
- **Plugin Version**: Shows current version (read-only)

### 🔹 Basic Settings  
- **NDI Source Name**: Name as it appears on network
- **Enable NDI Output**: Turn streaming on/off
- **Frame Rate**: Output frame rate

### 🔹 Performance Settings
- **GPU Acceleration**: Enable Metal GPU acceleration
- **Asynchronous Sending**: Enable async frame processing  
- **Optimal Color Format**: Use UYVY for best performance

### 🔹 HDR Settings
- **Enable HDR**: Turn on HDR output
- **Color Space**: Rec.709/Rec.2020/DCI-P3
- **Transfer Function**: SDR/PQ/HLG
- **Max Content Light Level**: HDR brightness (nits)
- **Max Frame Average Light Level**: HDR average (nits)

## ⚠️ Troubleshooting

### Plugin Not Visible:
1. Check `/Library/OFX Plugins/` contains `NDIOutput.ofx.bundle`
2. Restart DaVinci Resolve completely
3. Check Console.app for any error messages

### Version Not Showing:
1. Ensure you're using the latest v1.2.1 plugin
2. Look for "Plugin Information" group at the top of the inspector
3. The version should be clearly visible as a text field

### Performance Issues:
1. Enable **GPU Acceleration** for best performance
2. Enable **Optimal Color Format** for UYVY output
3. Check Console.app for GPU acceleration status logs

## 📞 Support

If you encounter issues:
1. Check the Console.app for "NDI Plugin" messages
2. Verify your system has Metal 3 support
3. Ensure NDI Advanced SDK is properly installed

## 🎯 What's New in v1.2.1

- ✅ Prominent version display in interface
- ✅ Organized parameter groups
- ✅ Better user experience
- ✅ All GPU acceleration features from v1.1.9
- ✅ Enhanced interface organization 