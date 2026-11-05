# ESP32-C3 Real-Time GPS Geofencing & Live Web Dashboard

A standalone IoT geofencing and asset-tracking system built on the **ESP32-C3** with a custom raw NMEA parser, spherical Haversine distance engine, hysteresis boundary protection, and an interactive **Leaflet.js** web dashboard.

![Dashboard Screenshot](<img width="1871" height="835" alt="Screenshot 2026-10-31 145139" src="https://github.com/user-attachments/assets/c7db17cd-f6e4-47f6-bdba-f7919bd231d1" />
)

## Key Features
* **Custom Zero-Library NMEA Parser:** Parses `$GGA` and `$GSV` sentences directly over Hardware UART (`9600` baud) with hexadecimal XOR checksum verification.
* **Multi-Constellation Tracking:** Monitors satellites in view, active locks, and max SNR (`dB-Hz`) across GPS, GLONASS, Galileo, and BeiDou.
* **Haversine Geofencing with Hysteresis:** Calculates real-time distance from a calibrated or preset Home coordinate (`30m` outer trigger radius, `27m` inner clear radius) to prevent GPS drift false alarms.
* **Dual-Mode Wi-Fi (STA + SoftAP Fallback):** Connects to campus/router Wi-Fi or automatically launches an `ESP32_GPS_DASHBOARD` hotspot (`192.168.4.1`).
* **Interactive Leaflet Map & Indoor Simulation:** Supports live coordinate plotting, OpenStreetMap Nominatim landmark search, and click-to-inject test coordinates via `/setcoords`.

## Hardware Pinout (ESP32-C3)
| Component | ESP32-C3 Pin | Notes |
| :--- | :--- | :--- |
| **GPS TX** | `GPIO 2` (RX) | 9600 Baud UART |
| **GPS RX** | `GPIO 3` (TX) | 9600 Baud UART |
| **Buzzer / Alert** | `GPIO 4` | Active `HIGH` on geofence breach |
| **Power / Serial** | `USB Type-C` | 115200 Baud Serial Monitor |

## Quick Start
1. Open `esp32-c3-gps-geofencing.ino` in the **Arduino IDE**.
2. Select **ESP32C3 Dev Module** under *Tools -> Board*.
3. Update `WIFI_SSID` and `WIFI_PASSWORD` with your network credentials.
4. Upload the sketch and open the **Serial Monitor** at `115200` baud to view the dashboard IP address.


CONTRIBUTORS:
1)Aadi Gautam
2)Manvi Gola
