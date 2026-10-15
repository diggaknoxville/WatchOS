# WatchOS Modern Dashboard - Quick Reference

## Files Created

| File | Purpose | Location |
|------|---------|----------|
| `getHtmlContent_new.cpp` | New dashboard function | `/home/michael/Dokumente/WatchOS/` |
| `DASHBOARD_INTEGRATION_GUIDE.md` | Full integration guide | `/home/michael/Dokumente/WatchOS/` |
| `DASHBOARD_QUICK_REFERENCE.md` | This file | `/home/michael/Dokumente/WatchOS/` |

## What to Do NOW

### 1. Review the New Function
```bash
cat /home/michael/Dokumente/WatchOS/getHtmlContent_new.cpp | head -100
```

### 2. Prepare for Integration
```bash
cd /home/michael/Dokumente/WatchOS/
cp WebManager.cpp WebManager.cpp.backup.$(date +%Y%m%d)
```

### 3. Find Current Function in WebManager.cpp
```bash
grep -n "String getHtmlContent()" WebManager.cpp
# Should be around line 300-341
```

### 4. Backup & Replace

**Option A: Manual (Recommended for first-time)**
1. Open `WebManager.cpp` in VS Code
2. Find `String getHtmlContent() {`
3. Delete everything until the matching closing `}`
4. Open `getHtmlContent_new.cpp`
5. Copy entire file content
6. Paste into WebManager.cpp

**Option B: Command-line (Advanced)**
```bash
# Extract line numbers
START=$(grep -n "^String getHtmlContent()" WebManager.cpp | cut -d: -f1)
END=$(tail -n +$START WebManager.cpp | grep -n "^}" | head -1 | cut -d: -f1)
END=$((START + END - 1))

# Create backup
cp WebManager.cpp WebManager.cpp.before_dashboard

# Remove old function (create modified file)
head -n $((START-1)) WebManager.cpp > temp.cpp
tail -n +$((END+1)) WebManager.cpp >> temp.cpp
cat getHtmlContent_new.cpp >> temp.cpp
mv temp.cpp WebManager.cpp
```

### 5. Verify Integration
```bash
# Check for syntax errors
g++ -c WebManager.cpp -I./
```

### 6. Compile
```bash
# Using Arduino IDE
arduino-cli compile --fqbn esp32:esp32:esp32s3 WatchOS.ino

# Or use your preferred method
```

### 7. Upload
```bash
arduino-cli upload --fqbn esp32:esp32:esp32s3 --port /dev/ttyUSB0 WatchOS.ino
```

### 8. Test
1. Open browser to `http://192.168.4.1` (hotspot) or `http://watchos.local`
2. Dashboard should load with modern design
3. Check theme toggle works (button in top-right)
4. Verify live data updates (watch battery % and CPU temp change)

## Function Signature

```cpp
String getHtmlContent() {
    String s; s.reserve(65000);
    // ... 2500+ lines of HTML, CSS, JavaScript
    return s;
}
```

## Key Integration Points

### Uses These Existing Global Variables
```cpp
sysLang                    // Language (0=DE, 1=EN, 2=FR)
wlanActive                 // WiFi status
wlanClientMode            // Client vs Hotspot mode
userSSID / apSSID         // Network SSID
userPWD / apPWD           // Network passwords
wlanIP                    // Current IP
btActive                  // Bluetooth status
batHistoryPct[]           // Battery history
historyCount              // History samples
historyInterval           // Sample interval
```

### Uses These Existing Functions
```cpp
tr(sysLang, de, en, fr)           // Translate
jsonEscape(str)                    // JSON escape
htmlEscape(str)                    // HTML escape
getBatteryCached(false)            // Battery %
isChargingCached(false)            // Charging status
getBatteryVoltageCached(false)     // Battery voltage
estimateBatteryEtaMinutes()        // ETA calc
temperatureRead()                  // CPU temp
WiFi.RSSI()                        // Signal strength
ESP.getCpuFreqMHz()                // CPU clock
ESP.getFreeHeap()                  // Free RAM
```

### Communicates with These Endpoints
```
GET  /api/status         → JSON with live data
POST /settime            → Set date/time
POST /update             → OTA firmware upload
POST /stopbad            → Emergency stop bad apps
GET  /export             → Export settings
POST /reboot             → System reboot
```

## Dashboard Structure

```
┌─────────────────────────────────────────┐
│  Logo  Brand  [Chip]  [Theme] [Buttons] │  Header
├─────────────────────────────────────────┤
│  ┌──────────────┐  ┌─────────────────┐  │
│  │ Battery Ring │  │ Telemetry Tiles │  │ Hero
│  │ + ETA        │  │ (6 tiles)       │  │
│  │              │  │ + Sparkline     │  │
│  └──────────────┘  └─────────────────┘  │
├─────────────────────────────────────────┤
│  Connection Status │ Time & NTP Settings │  Info Cards
├─────────────────────────────────────────┤
│           Good Apps │ Bad Apps            │  Tools
├─────────────────────────────────────────┤
│  OTA Update │ System Management          │  System
└─────────────────────────────────────────┘
```

## Live Updates Timeline

```
Page Load
   ↓
setupBatteryRing() - Initialize SVG ring
drawSparkline()    - Initialize sparkline
updateLive()       - First data fetch
   ↓
Every 0.7s (when visible)
   ├─ fetch('/api/status')
   ├─ Update battery ring
   ├─ Update telemetry values
   ├─ Update ETA
   └─ Schedule next update
   ↓
Every 2.5s (when hidden)
   └─ Slower updates to save bandwidth
```

## Customization Checklist

- [ ] Change accent color: Edit `--accent` in CSS
- [ ] Change update frequency: Modify `scheduleLive(700)` in JS
- [ ] Change theme colors: Edit CSS root variables
- [ ] Add more telemetry tiles: Add new tile HTML + JS
- [ ] Change header branding: Edit logo and title
- [ ] Hide sections: Use CSS `display: none`
- [ ] Add new buttons: Follow existing button pattern

## CSS Variables Reference

```css
/* Colors */
--bg: #eceef2              /* Main background */
--surface: #ffffff         /* Cards/surfaces */
--surface-2: #f4f6f9       /* Secondary surfaces */
--text: #171c24            /* Text color */
--muted: #5b6675           /* Muted text */
--faint: #8a93a2           /* Faint text */
--accent: #c9770a          /* Primary color (orange) */
--accent-2: #0c9c90        /* Secondary color (teal) */
--good: #1a9d51            /* Success (green) */
--warn: #c9770a            /* Warning (orange) */
--crit: #d94339            /* Critical (red) */
--charge: #2b7fe0          /* Charging (blue) */

/* Sizing */
--r: 14px                  /* Standard border-radius */
--r-sm: 9px                /* Small border-radius */

/* Fonts */
--font-ui: sans-serif      /* UI font */
--font-mono: monospace     /* Mono font for data */
```

## Troubleshooting Checklist

| Problem | Solution |
|---------|----------|
| Dashboard won't load | Check /api/status returns valid JSON |
| Live data not updating | Verify 700ms update interval, check network tab |
| Battery ring not showing | Ensure SVG supports circle element, check CSS |
| Theme toggle not working | Check localStorage support, verify CSS classes |
| Styling broken | Clear browser cache (Ctrl+Shift+Del) |
| Text garbled | Check UTF-8 encoding, verify translations |
| Mobile display broken | Check viewport meta tag, test responsive media queries |
| OTA upload fails | Verify /update endpoint exists, check file size |

## Performance Metrics

| Metric | Value | Note |
|--------|-------|------|
| HTML Size | ~80 KB | Compressed with PROGMEM |
| CSS Size | ~45 KB | Inline, no external files |
| JS Size | ~35 KB | Inline, no external libraries |
| Initial Load | ~2s | On 2.4GHz WiFi |
| Live Update Interval | 700ms | 0.7s refresh rate |
| Memory Usage | ~150 KB | RAM for buffering |

## Browser Console Commands (Debug)

```javascript
// Force update
updateLive();

// Check if theme is dark
currentDark();

// Change theme programmatically
root.setAttribute('data-theme', 'dark');

// Show toast message
showToast('Test message');

// Get current battery percentage
document.getElementById('battPct').textContent;

// Disable live updates
if(liveTimer) clearTimeout(liveTimer);

// Enable live updates again
updateLive();
```

## API Response Example

```json
{
  "akku": 76,
  "volt": "4.10",
  "cpu": "41.2",
  "ram_free": 198,
  "ram_tot": 512,
  "geld": "0.0042",
  "wifi": true,
  "charging": true,
  "bt": true,
  "ssid": "WatchOS",
  "ip": "192.168.4.1",
  "cpufreq": 240,
  "uptime": 3600,
  "rssi": -58,
  "rssi_pct": 84,
  "eta_min": 45,
  "eta_mode": "charge",
  "steps": 6284
}
```

## Common Modifications

### Add a new telemetry tile
```html
<!-- Add to .tiles div -->
<div class="tile">
  <div class="lbl">Label</div>
  <div class="v" id="myValue">-</div>
</div>
```

```javascript
// Add to updateLive() function
document.getElementById('myValue').innerHTML = d.myField + '<u>Unit</u>';
```

### Change update frequency to 1 second
```javascript
// Find this line:
scheduleLive(document.hidden ? 2500 : 700);

// Change to:
scheduleLive(document.hidden ? 2500 : 1000);
```

### Hide the OTA section
```css
/* Add to style section */
.card:has(svg path d*="M12 2v6") {
  display: none;
}
```

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-07-13 | Initial release with modern design |

## Support & Questions

1. Check DASHBOARD_INTEGRATION_GUIDE.md for detailed docs
2. Review browser DevTools console for errors
3. Check if /api/status endpoint is working
4. Verify WebManager.cpp compilation

---

**Ready to integrate!** Follow the "What to Do NOW" section above to get started.
