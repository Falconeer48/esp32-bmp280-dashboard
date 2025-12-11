# WiFi Log Entries - How to View

## Log Entries Created by WiFi Reconnection Code

The WiFi reconnection code creates several log entries that show when WiFi needs to be restarted:

### 1. **WiFi Disconnection Detected**
```
WiFi disconnected! Status: [status_code]
RSSI: [signal_strength] dBm
```
- **When**: Every 10 seconds when WiFi is disconnected
- **What it means**: WiFi connection has been lost
- **Status codes**: 
  - `3` = WL_DISCONNECTED
  - `4` = WL_CONNECTION_LOST
  - `6` = WL_CONNECT_FAILED

### 2. **Reconnection Attempt**
```
Attempting WiFi reconnect (attempt [number])...
```
- **When**: Every 30 seconds when WiFi is disconnected
- **What it means**: Trying to reconnect to WiFi
- **Shows**: Attempt number (1-5, then resets)

### 3. **Reconnection Progress**
```
.....
```
- **When**: During reconnection attempt
- **What it means**: Waiting for WiFi to connect (up to 10 seconds)
- **Each dot**: 0.5 seconds

### 4. **Successful Reconnection**
```
WiFi reconnected! IP: [ip_address]
RSSI: [signal_strength] dBm
```
- **When**: WiFi successfully reconnects
- **What it means**: WiFi is back online
- **Shows**: New IP address and signal strength

### 5. **Reconnection Failed**
```
WiFi reconnect failed.
Max reconnect attempts reached. Will try again later.
```
- **When**: After 5 failed attempts
- **What it means**: Will wait 30 seconds before trying again

### 6. **WiFi Status (Periodic)**
```
WiFi OK - IP: [ip_address], RSSI: [signal_strength] dBm
```
- **When**: Every 5 minutes when WiFi is connected
- **What it means**: WiFi is stable and working
- **Note**: Only shown if `SERIAL_VERBOSE` is `true`

### 7. **MQTT Reconnection**
```
Attempting MQTT connection...
MQTT connected
```
- **When**: After WiFi reconnects, if MQTT is disconnected
- **What it means**: MQTT is reconnecting after WiFi comes back

## How to View Logs

### Method 1: Serial Monitor (Arduino IDE)
1. **Open Arduino IDE**
2. **Connect ESP32** via USB
3. **Tools** → **Serial Monitor**
4. **Set baud rate**: 115200
5. **Watch for log entries** in real-time

### Method 2: Serial Monitor (PlatformIO)
1. **Open PlatformIO**
2. **Connect ESP32** via USB
3. **Click Serial Monitor** icon
4. **Set baud rate**: 115200
5. **Watch for log entries**

### Method 3: Command Line (screen/minicom)
```bash
# macOS/Linux
screen /dev/ttyUSB0 115200

# Or with minicom
minicom -D /dev/ttyUSB0 -b 115200
```

### Method 4: Home Assistant Logs
If you have Home Assistant monitoring the sensor:
- **Settings** → **System** → **Logs**
- Look for MQTT disconnection events
- Check `binary_sensor.bmp280_status` state changes

## What to Look For

### Normal Operation
- `WiFi OK` messages every 5 minutes
- No disconnection messages
- Stable RSSI values

### WiFi Issues
- Frequent `WiFi disconnected!` messages
- `Attempting WiFi reconnect` messages
- Low RSSI values (< -70 dBm)
- Multiple reconnection attempts

### Critical Issues
- `Max reconnect attempts reached` messages
- WiFi never reconnects
- MQTT stays disconnected

## Log Entry Examples

### Example 1: Normal WiFi Drop and Recovery
```
WiFi disconnected! Status: 3
RSSI: -75 dBm
Attempting WiFi reconnect (attempt 1)...
.....
WiFi reconnected! IP: 192.168.50.100
RSSI: -65 dBm
Attempting MQTT connection...
MQTT connected
```

### Example 2: Multiple Reconnection Attempts
```
WiFi disconnected! Status: 3
RSSI: -80 dBm
Attempting WiFi reconnect (attempt 1)...
WiFi reconnect failed.
Attempting WiFi reconnect (attempt 2)...
WiFi reconnect failed.
...
Max reconnect attempts reached. Will try again later.
```

### Example 3: Stable Connection
```
WiFi OK - IP: 192.168.50.100, RSSI: -55 dBm
WiFi OK - IP: 192.168.50.100, RSSI: -55 dBm
```

## Troubleshooting Based on Logs

### If you see frequent disconnections:
1. **Check RSSI**: Should be > -70 dBm
2. **Check power supply**: Voltage drops cause WiFi drops
3. **Check router**: May need restart
4. **Check interference**: Other devices on 2.4GHz

### If reconnection fails:
1. **Check WiFi credentials**: SSID and password correct?
2. **Check router settings**: Client isolation disabled?
3. **Check signal strength**: Move ESP32 closer to router
4. **Check power supply**: Stable 5V supply?

## Filtering Logs

### In Serial Monitor:
- Use **Filter** box to search for:
  - `WiFi disconnected`
  - `WiFi reconnected`
  - `RSSI`
  - `attempt`

### Save Logs to File:
1. **Serial Monitor** → **Settings** (gear icon)
2. **Enable "Show timestamp"**
3. **Copy/paste** logs to text file
4. Or use command line tools to save directly

## Summary

**Log entries show:**
- ✅ When WiFi disconnects
- ✅ Reconnection attempts
- ✅ Successful reconnections
- ✅ Signal strength (RSSI)
- ✅ IP address changes
- ✅ MQTT reconnection status

**View logs via:**
- Serial Monitor (115200 baud)
- Home Assistant logs
- Command line tools

**Key indicators:**
- Frequent disconnections = WiFi stability issue
- Low RSSI = Signal strength problem
- Failed reconnections = Network/power issue


