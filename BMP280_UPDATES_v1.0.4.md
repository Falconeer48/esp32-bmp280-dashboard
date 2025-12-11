# BMP280 Sensor Sketch Updates - Version 1.0.4

## Changes Made

### 1. WiFi Dropout Logging ✅
- **Added WiFi dropout event logging** to track when and why WiFi disconnects
- Logs store up to 20 recent dropout events with:
  - Timestamp (epoch seconds)
  - WiFi status code
  - RSSI at time of dropout
  - How long WiFi was connected before dropout
  - Whether reconnection succeeded
  - How long reconnection took
- **New endpoint**: `/wifi-log` - Returns JSON array of WiFi dropout logs
- **Serial output**: Enhanced logging shows detailed dropout/reconnection information

**How to view WiFi logs:**
- **Serial Monitor**: Look for "=== WiFi DROPOUT LOGGED ===" and "=== WiFi RECONNECTED ===" messages
- **Web API**: Access `http://<ESP32-IP>/wifi-log` to get JSON log data
- **Example log entry:**
  ```json
  {
    "timestamp": 1234567890,
    "statusCode": 6,
    "rssi": -75,
    "connectedDuration": 3600,
    "reconnected": true,
    "reconnectTime": 15
  }
  ```

### 2. Fixed Y-Axis Auto-Scaling ✅
- **Temperature Y-axis** now automatically calculates min/max based on **all visible data points** (not just current value)
- Ensures temperature readings are **always within 2°C** of the visible range
- Uses at least 2°C padding, or 10% of the data range (whichever is larger)
- **Pressure Y-axis** uses similar logic with 5 hPa minimum padding
- Fixes issue where data below the minimum value wasn't visible

**How it works:**
- When data loads, finds min/max of all valid temperature readings in the selected time range
- Sets Y-axis to `min - 2°C` and `max + 2°C` (or 10% padding if range is larger)
- Ensures all data points are always visible on the graph

### 3. Added 3h/12h/24h Time Range Buttons ✅
- **Added time range selector buttons** matching the fridge/freezer sketch
- Three buttons: "Last 3h", "Last 12h", "Last 24h"
- **Reset zoom button** to restore default view after panning/zooming
- Graph data storage increased from 240 to **480 readings** (supports full 24 hours)
- Client-side filtering - all data sent to browser, JavaScript filters by selected range

**How to use:**
- Click "Last 3h", "Last 12h", or "Last 24h" buttons to change time range
- Active button is highlighted in purple
- Click "Reset zoom" to restore default view after zooming/panning
- Graph automatically updates Y-axis scaling based on visible data

### 4. Additional Improvements
- **Chart.js plugins**: Added `chartjs-adapter-date-fns` and `chartjs-plugin-zoom` for better time axis and zoom functionality
- **Time-based X-axis**: Chart now uses proper time scale instead of labels
- **Improved zoom/pan**: Mouse wheel zoom and drag-to-pan enabled

## Version Information
- **Previous Version**: 1.0.3
- **New Version**: 1.0.4
- **Date**: Updated with WiFi logging, Y-axis fixes, and time range buttons

## Testing Checklist
- [ ] Upload sketch to ESP32
- [ ] Verify WiFi dropout logging appears in Serial Monitor when WiFi disconnects
- [ ] Check `/wifi-log` endpoint returns JSON data
- [ ] Verify temperature graph shows all data (not cut off at bottom)
- [ ] Test 3h/12h/24h buttons - verify graph updates correctly
- [ ] Test zoom/pan functionality
- [ ] Verify Y-axis auto-scales correctly for both temperature and pressure

## Notes
- WiFi logs are stored in RAM and will be lost on reboot (max 20 entries)
- Graph data is stored in RAM (480 readings = ~24 hours at 3-minute intervals)
- If ESP32 reboots, graph history and WiFi logs are reset
- For persistent logging, consider adding SD card or EEPROM storage (future enhancement)


