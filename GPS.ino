#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <math.h>
#include <stdlib.h>

// =====================================================
// 1. WIFI SETTINGS
// =====================================================

const char *WIFI_SSID = "DSEU-FACULTY-WiFi";
const char *WIFI_PASSWORD = "DseU@123##";

// ESP32 hotspot fallback
const char *AP_SSID = "ESP32_GPS_DASHBOARD";
const char *AP_PASSWORD = "gpsdashboard123";

// =====================================================
// 2. GPS AND ESP32-C3 CONNECTION
// =====================================================

HardwareSerial gpsSerial(1);
WebServer server(80);

// GPS TX -> ESP32 GPIO 2
// GPS RX -> ESP32 GPIO 3
const int GPS_RX_PIN = 2;
const int GPS_TX_PIN = 3;
const int ALERT_PIN = 4;

const int GPS_BAUD = 9600;

// =====================================================
// 3. GEOFENCE SETTINGS & INITIAL COORDINATES
// =====================================================

const double FENCE_RADIUS_METERS = 30.0;
const double FENCE_CLEAR_METERS = 27.0;

const int HOME_SAMPLES = 10;
const int MIN_SATS_HOME = 4;

const unsigned long GPS_TIMEOUT = 5000;

// Default coordinates (DSEU Dwarka Campus, New Delhi)
double homeLat = 28.581930;
double homeLon = 77.062996;

// =====================================================
// 4. GLOBAL VARIABLES
// =====================================================

double sumLat = 0;
double sumLon = 0;

// Initialize currentLat and currentLon with default coordinates
double currentLat = 28.581930;
double currentLon = 77.062996;
double currentDistance = 0;

int homeCount = 0;
int satellites = 0;

bool homeIsSet = true;      // Default home coordinates active immediately
bool gpsHasFix = false;
bool manualSimulatedFix = false; 
bool outside = false;
bool hotspotMode = false;

unsigned long lastFixMillis = 0;
unsigned long lastStatusMillis = 0;
unsigned long lastNmeaMillis = 0;

unsigned long bytesReceived = 0;
unsigned long nmeaCount = 0;
unsigned long badChecksumCount = 0;

String nmeaBuffer = "";
String utcTime = "--";

// Satellite information:
// 0 = GPS, 1 = GLONASS, 2 = Galileo, 3 = BeiDou

int satsInView[4] = {0, 0, 0, 0};
int satsTracked[4] = {0, 0, 0, 0};
int snrMax[4] = {0, 0, 0, 0};

int trackedAcc[4] = {0, 0, 0, 0};
int snrAcc[4] = {0, 0, 0, 0};

// =====================================================
// 5. NMEA HELPER FUNCTIONS
// =====================================================

double nmeaToDecimal(String coord, String direction) {
  if (coord.length() < 4) return 0;

  double value = coord.toDouble();
  int degrees = (int)(value / 100.0);
  double minutes = value - degrees * 100.0;
  double decimal = degrees + minutes / 60.0;

  if (direction == "S" || direction == "W") {
    decimal = -decimal;
  }

  return decimal;
}

// -----------------------------------------------------

double distanceInMeters(
  double lat1,
  double lon1,
  double lat2,
  double lon2
) {
  const double R = 6371000.0;

  double dLat = (lat2 - lat1) * PI / 180.0;
  double dLon = (lon2 - lon1) * PI / 180.0;

  double a =
    sin(dLat / 2.0) * sin(dLat / 2.0) +
    cos(lat1 * PI / 180.0) *
    cos(lat2 * PI / 180.0) *
    sin(dLon / 2.0) * sin(dLon / 2.0);

  if (a > 1.0) a = 1.0;
  if (a < 0.0) a = 0.0;

  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

  return R * c;
}

// -----------------------------------------------------

void evaluateGeofence() {
  currentDistance = distanceInMeters(
    currentLat,
    currentLon,
    homeLat,
    homeLon
  );

  if (!outside && currentDistance > FENCE_RADIUS_METERS) {
    outside = true;
    Serial.println("GEOFENCE ALERT: OUTSIDE!");
  } else if (outside && currentDistance < FENCE_CLEAR_METERS) {
    outside = false;
    Serial.println("GEOFENCE STATUS: SAFE");
  }

  digitalWrite(ALERT_PIN, outside ? HIGH : LOW);
}

// -----------------------------------------------------

bool checksumOk(const String &line) {
  int star = line.indexOf('*');

  if (line.length() < 1 ||
      line.charAt(0) != '$' ||
      star < 0 ||
      star + 2 >= (int)line.length()) {
    return false;
  }

  uint8_t calculated = 0;

  for (int i = 1; i < star; i++) {
    calculated ^= (uint8_t)line.charAt(i);
  }

  char checksumText[3];
  checksumText[0] = line.charAt(star + 1);
  checksumText[1] = line.charAt(star + 2);
  checksumText[2] = '\0';

  char *endPtr;
  long received = strtol(checksumText, &endPtr, 16);

  if (endPtr != checksumText + 2) return false;

  return calculated == (uint8_t)received;
}

// -----------------------------------------------------

int splitFields(
  const String &sentence,
  String *fields,
  int maxFields
) {
  int count = 0;
  int start = 0;

  for (int i = 0; i <= (int)sentence.length() && count < maxFields; i++) {
    if (i == (int)sentence.length() || sentence.charAt(i) == ',') {
      fields[count++] = sentence.substring(start, i);
      start = i + 1;
    }
  }

  return count;
}

// -----------------------------------------------------

int talkerIndex(const String &line) {
  String talker = line.substring(1, 3);

  if (talker == "GP") return 0;
  if (talker == "GL") return 1;
  if (talker == "GA") return 2;
  if (talker == "GB" || talker == "BD") return 3;

  return -1;
}

// -----------------------------------------------------

String formatUtc(const String &t) {
  if (t.length() < 6) return "--";

  return t.substring(0, 2) + ":" +
         t.substring(2, 4) + ":" +
         t.substring(4, 6);
}

// -----------------------------------------------------

int totalInView() {
  int total = 0;
  for (int i = 0; i < 4; i++) {
    total += satsInView[i];
  }
  return total;
}

// -----------------------------------------------------

int totalTracked() {
  int total = 0;
  for (int i = 0; i < 4; i++) {
    total += satsTracked[i];
  }
  return total;
}

// -----------------------------------------------------

int bestSnr() {
  int best = 0;
  for (int i = 0; i < 4; i++) {
    if (snrMax[i] > best) best = snrMax[i];
  }
  return best;
}

// -----------------------------------------------------

bool nmeaDataOk() {
  return nmeaCount > 0 && (millis() - lastNmeaMillis < 3000);
}

// =====================================================
// 6. GSV SATELLITE INFORMATION
// =====================================================

void handleGSV(
  const String &line,
  String *field,
  int count
) {
  int index = talkerIndex(line);

  if (index < 0 || count < 4) return;

  int numMsg = field[1].toInt();
  int msgNum = field[2].toInt();

  satsInView[index] = field[3].toInt();

  if (msgNum == 1) {
    trackedAcc[index] = 0;
    snrAcc[index] = 0;
  }

  for (int g = 4; g + 3 < count; g += 4) {
    if (field[g + 3].length() > 0) {
      int snr = field[g + 3].toInt();
      if (snr > 0) {
        trackedAcc[index]++;
        if (snr > snrAcc[index]) {
          snrAcc[index] = snr;
        }
      }
    }
  }

  if (msgNum >= numMsg) {
    satsTracked[index] = trackedAcc[index];
    snrMax[index] = snrAcc[index];
  }
}

// =====================================================
// 7. GGA POSITION AND GEOFENCING
// =====================================================

void handleGGA(String *field, int count) {
  if (count < 8) return;

  utcTime = formatUtc(field[1]);
  int fixQuality = field[6].toInt();
  satellites = field[7].toInt();

  if (
    fixQuality <= 0 ||
    field[2].length() == 0 ||
    field[3].length() == 0 ||
    field[4].length() == 0 ||
    field[5].length() == 0
  ) {
    if (!manualSimulatedFix) {
      gpsHasFix = false;
      digitalWrite(ALERT_PIN, LOW);
    }
    return;
  }

  double newLat = nmeaToDecimal(field[2], field[3]);
  double newLon = nmeaToDecimal(field[4], field[5]);

  if (
    newLat < -90.0 || newLat > 90.0 ||
    newLon < -180.0 || newLon > 180.0 ||
    (newLat == 0.0 && newLon == 0.0)
  ) {
    if (!manualSimulatedFix) {
      gpsHasFix = false;
    }
    return;
  }

  currentLat = newLat;
  currentLon = newLon;
  gpsHasFix = true;
  manualSimulatedFix = false;
  lastFixMillis = millis();

  // ---------------------------------------------------
  // HOME CALIBRATION
  // ---------------------------------------------------

  if (!homeIsSet) {
    if (satellites >= MIN_SATS_HOME) {
      sumLat += currentLat;
      sumLon += currentLon;
      homeCount++;

      Serial.print("Home calibration: ");
      Serial.print(homeCount);
      Serial.print("/");
      Serial.println(HOME_SAMPLES);

      if (homeCount >= HOME_SAMPLES) {
        homeLat = sumLat / homeCount;
        homeLon = sumLon / homeCount;
        homeIsSet = true;

        Serial.println();
        Serial.println("================================");
        Serial.println("HOME LOCATION CALIBRATED");
        Serial.print("HOME LATITUDE: ");
        Serial.println(homeLat, 6);
        Serial.print("HOME LONGITUDE: ");
        Serial.println(homeLon, 6);
        Serial.println("================================");
      }
    }
    return;
  }

  // ---------------------------------------------------
  // DISTANCE & GEOFENCE CALCULATION
  // ---------------------------------------------------

  evaluateGeofence();

  // ---------------------------------------------------
  // SERIAL OUTPUT
  // ---------------------------------------------------

  Serial.print("Latitude: ");
  Serial.print(currentLat, 6);
  Serial.print(" | Longitude: ");
  Serial.print(currentLon, 6);
  Serial.print(" | Satellites: ");
  Serial.print(satellites);
  Serial.print(" | Distance: ");
  Serial.print(currentDistance, 1);
  Serial.print(" m | ");
  Serial.println(outside ? "OUTSIDE ALERT" : "SAFE");
}

// =====================================================
// 8. PROCESS GPS SENTENCES
// =====================================================

void processGPS(String line) {
  line.trim();

  if (line.length() < 6 || line.charAt(0) != '$') {
    return;
  }

  if (!checksumOk(line)) {
    badChecksumCount++;
    return;
  }

  nmeaCount++;
  lastNmeaMillis = millis();

  int star = line.indexOf('*');
  line = line.substring(0, star);

  String type = line.substring(3, 6);

  if (type != "GGA" && type != "GSV") {
    return;
  }

  String field[24];
  int count = splitFields(line, field, 24);

  if (type == "GGA") {
    handleGGA(field, count);
  } else if (type == "GSV") {
    handleGSV(line, field, count);
  }
}

// =====================================================
// 9. GPS STATUS REPORT
// =====================================================

void printStatus() {
  Serial.println();
  Serial.println("--------- GPS STATUS ---------");

  if (bytesReceived == 0) {
    Serial.println("NO DATA FROM GPS MODULE");
    Serial.println("Check GPS wiring and power.");
  } else if (!nmeaDataOk()) {
    Serial.println("GPS DATA NOT VALID");
    Serial.print("Bad checksums: ");
    Serial.println(badChecksumCount);
  } else {
    Serial.println("GPS MODULE LINK: OK");
    Serial.print("UTC TIME: ");
    Serial.println(utcTime);
    Serial.print("SATELLITES IN VIEW: ");
    Serial.println(totalInView());
    Serial.print("SATELLITES WITH SIGNAL: ");
    Serial.println(totalTracked());
    Serial.print("BEST SIGNAL: ");
    Serial.print(bestSnr());
    Serial.println(" dB-Hz");
    Serial.print("SATELLITES USED: ");
    Serial.println(satellites);

    if (totalInView() == 0) {
      Serial.println("No satellites detected.");
      Serial.println("Check GPS antenna.");
      Serial.println("Place antenna outdoors.");
    }
  }

  Serial.print("Active Lat/Lon: ");
  Serial.print(currentLat, 6);
  Serial.print(", ");
  Serial.println(currentLon, 6);
  Serial.print("Home Lat/Lon:   ");
  Serial.print(homeLat, 6);
  Serial.print(", ");
  Serial.println(homeLon, 6);
  Serial.println("------------------------------");
}

// =====================================================
// 10. WEB DASHBOARD HTML
// =====================================================

const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 GPS Dashboard</title>

<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>

<style>
* {
  box-sizing: border-box;
}

body {
  margin: 0;
  font-family: Arial, sans-serif;
  background: #eef2f7;
  text-align: center;
}

header {
  background: #142d4e;
  color: white;
  padding: 20px;
}

.container {
  padding: 15px;
  max-width: 1100px;
  margin: 0 auto;
}

.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 12px;
}

.card {
  background: white;
  padding: 20px 10px;
  border-radius: 12px;
  box-shadow: 0 2px 8px #0002;
  min-height: 110px;
}

.value {
  font-size: 20px;
  font-weight: bold;
  margin-top: 12px;
  color: #1769aa;
  overflow-wrap: anywhere;
}

.small {
  font-size: 12px;
  color: #666;
  margin-top: 6px;
}

.safe { color: green; }
.alert { color: red; }
.waiting { color: #1769aa; }

#hint {
  margin: 18px auto;
  max-width: 700px;
  padding: 12px;
  background: #fff4d6;
  color: #7a5200;
  border-radius: 10px;
  display: none;
}

#map {
  height: 450px;
  margin-top: 20px;
  border-radius: 12px;
  border: 2px solid white;
}

.search-container, .coord-panel {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  max-width: 750px;
  margin: 14px auto 8px;
  justify-content: center;
}

.coord-panel {
  background: white;
  padding: 14px;
  border-radius: 12px;
  box-shadow: 0 2px 8px #0001;
}

.search-container input, .coord-panel input {
  flex: 1;
  min-width: 140px;
  padding: 10px 12px;
  border: 1px solid #b8c4d1;
  border-radius: 8px;
  font-size: 15px;
}

button {
  padding: 10px 16px;
  border: 0;
  border-radius: 8px;
  background: #1769aa;
  color: white;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
}

button:hover {
  background: #12558a;
}

button.secondary {
  background: #2e7d32;
}

button.secondary:hover {
  background: #1b5e20;
}

#searchStatus {
  min-height: 20px;
  margin: 8px auto;
  max-width: 700px;
  overflow-wrap: anywhere;
  color: #444;
}

a {
  display: inline-block;
  margin: 14px;
  color: #075eb5;
  font-weight: bold;
  text-decoration: none;
}

.footer {
  color: #555;
}
</style>
</head>

<body>

<header>
  <h2>ESP32-C3 GPS GEOFENCING</h2>
  <p>Live GPS Sensor Monitoring Dashboard</p>
</header>

<div class="container">

<div class="cards">
  <div class="card">
    GPS MODULE
    <div class="value waiting" id="link">WAITING</div>
  </div>

  <div class="card">
    GPS FIX
    <div class="value waiting" id="gps">WAITING</div>
  </div>

  <div class="card">
    SATELLITES IN VIEW
    <div class="value" id="inview">--</div>
    <div class="small" id="breakdown"></div>
  </div>

  <div class="card">
    SATELLITES USED
    <div class="value" id="sats">--</div>
  </div>

  <div class="card">
    BEST SIGNAL
    <div class="value" id="snr">--</div>
  </div>

  <div class="card">
    UTC TIME
    <div class="value" id="utc">--</div>
  </div>

  <div class="card">
    LATITUDE
    <div class="value" id="lat">28.581930</div>
    <div class="small" id="latSource">Default / Live</div>
  </div>

  <div class="card">
    LONGITUDE
    <div class="value" id="lon">77.062996</div>
    <div class="small" id="lonSource">Default / Live</div>
  </div>

  <div class="card">
    HOME COORDINATES
    <div class="value" style="font-size:15px" id="homeCoords">28.581930, 77.062996</div>
  </div>

  <div class="card">
    DISTANCE FROM HOME
    <div class="value" id="dist">0.0 m</div>
  </div>

  <div class="card">
    GEOFENCE STATUS
    <div class="value waiting" id="status">WAITING</div>
  </div>

  <div class="card">
    ALERT
    <div class="value" id="alert">OFF</div>
  </div>
</div>

<div id="hint"></div>

<h3>SET OR TEST LATITUDE & LONGITUDE</h3>
<div class="coord-panel">
  <input type="number" step="any" id="inputLat" placeholder="Latitude (e.g. 28.581930)" value="28.581930" />
  <input type="number" step="any" id="inputLon" placeholder="Longitude (e.g. 77.062996)" value="77.062996" />
  <button type="button" onclick="applyCoords('pos')">Set as Current Position</button>
  <button type="button" class="secondary" onclick="applyCoords('home')">Set as Home Fence</button>
</div>

<h3>LIVE GPS LOCATION MAP</h3>
<p>
  Click anywhere on the map to populate Latitude & Longitude above, or search a landmark below.
</p>

<div class="search-container">
  <input
    type="text"
    id="locationSearch"
    placeholder="Search location (e.g. India Gate, Delhi)"
    autocomplete="off"
  />
  <button type="button" onclick="searchLocation()">Search</button>
</div>

<p id="searchStatus" aria-live="polite"></p>

<div id="map"></div>

<a id="google" href="https://www.google.com/maps?q=28.581930,77.062996" target="_blank">
  OPEN CURRENT GPS LOCATION IN GOOGLE MAPS
</a>

<p id="coordinates">
  Current Coordinates: 28.581930, 77.062996
</p>

<p id="manualStatus">
  Click the map to select test coordinates.
</p>

<a id="manualGoogle" href="#" target="_blank" style="display:none">
  OPEN SELECTED PLACE IN GOOGLE MAPS
</a>

<p class="footer">
  Dashboard refreshes automatically every second.
</p>

</div>

<script>
let map = null;
let marker = null;
let homeMarker = null;
let fenceCircle = null;
let manualMarker = null;
let searchMarker = null;
let hasCentered = false;

const mapAvailable = typeof L !== "undefined";

if (mapAvailable) {
  map = L.map("map").setView([28.581930, 77.062996], 17);

  L.tileLayer(
    "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    {
      maxZoom: 19,
      attribution: "&copy; OpenStreetMap"
    }
  ).addTo(map);

  map.on("click", function (event) {
    const lat = event.latlng.lat;
    const lon = event.latlng.lng;

    document.getElementById("inputLat").value = lat.toFixed(6);
    document.getElementById("inputLon").value = lon.toFixed(6);

    if (!manualMarker) {
      manualMarker = L.marker(event.latlng)
        .addTo(map)
        .bindPopup("Selected Point");
    } else {
      manualMarker.setLatLng(event.latlng);
    }

    manualMarker.openPopup();

    document.getElementById("manualStatus").innerText =
      "Selected coordinates: " + lat.toFixed(6) + ", " + lon.toFixed(6);

    const manualGoogle = document.getElementById("manualGoogle");
    manualGoogle.href = "https://www.google.com/maps?q=" + lat + "," + lon;
    manualGoogle.style.display = "inline-block";
  });
} else {
  document.getElementById("map").style.display = "none";
  document.getElementById("manualStatus").innerText =
    "Map unavailable: check your internet connection, then reload the dashboard.";
}

async function applyCoords(mode) {
  const lat = parseFloat(document.getElementById("inputLat").value);
  const lon = parseFloat(document.getElementById("inputLon").value);
  const status = document.getElementById("searchStatus");

  if (!Number.isFinite(lat) || !Number.isFinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    status.innerText = "Please enter valid Latitude (-90 to 90) and Longitude (-180 to 180).";
    return;
  }

  try {
    const url = `/setcoords?lat=${encodeURIComponent(lat)}&lon=${encodeURIComponent(lon)}&mode=${encodeURIComponent(mode)}`;
    const res = await fetch(url, { cache: "no-store" });
    if (res.ok) {
      status.innerText = mode === "home"
        ? `Home Geofence updated to (${lat.toFixed(6)}, ${lon.toFixed(6)})`
        : `Current GPS position set to (${lat.toFixed(6)}, ${lon.toFixed(6)})`;
      updateDashboard();
    }
  } catch (err) {
    status.innerText = "Failed to send coordinates to ESP32.";
  }
}

async function searchLocation() {
  const input = document.getElementById("locationSearch");
  const status = document.getElementById("searchStatus");
  const place = input.value.trim();

  if (!place) {
    status.innerText = "Please enter a location.";
    input.focus();
    return;
  }

  status.innerText = "Searching...";

  try {
    const url =
      "https://nominatim.openstreetmap.org/search?format=json&limit=1&q=" +
      encodeURIComponent(place);

    const response = await fetch(url, {
      headers: { "Accept": "application/json" },
      cache: "no-store"
    });

    if (!response.ok) throw new Error("Search failed");

    const results = await response.json();

    if (!Array.isArray(results) || results.length === 0) {
      status.innerText = "Location not found. Try adding a city or country.";
      return;
    }

    const lat = Number.parseFloat(results[0].lat);
    const lon = Number.parseFloat(results[0].lon);
    const displayName = results[0].display_name || place;

    document.getElementById("inputLat").value = lat.toFixed(6);
    document.getElementById("inputLon").value = lon.toFixed(6);

    if (mapAvailable && map) {
      const searchPosition = [lat, lon];
      if (searchMarker) {
        searchMarker.setLatLng(searchPosition);
      } else {
        searchMarker = L.marker(searchPosition).addTo(map);
      }
      searchMarker.bindPopup(displayName).openPopup();
      map.setView(searchPosition, 16);
    }

    status.innerText =
      "Found: " + displayName + " (" + lat.toFixed(6) + ", " + lon.toFixed(6) + ")";
  } catch (error) {
    status.innerText = "Search failed. Check your internet connection.";
  }
}

document.getElementById("locationSearch").addEventListener("keydown", function (event) {
  if (event.key === "Enter") {
    searchLocation();
  }
});

async function updateDashboard() {
  try {
    const response = await fetch("/data", { cache: "no-store" });
    if (!response.ok) throw new Error("Server error");

    const d = await response.json();

    const link = document.getElementById("link");
    link.innerText = d.dataOk ? "DATA OK" : "NO DATA";
    link.className = d.dataOk ? "value safe" : "value alert";

    const gps = document.getElementById("gps");
    gps.innerText = d.fix ? (d.simulated ? "SIMULATED" : "FIX OK") : "DEFAULT COORDS";
    gps.className = d.fix ? "value safe" : "value waiting";

    document.getElementById("inview").innerText = d.inView;
    document.getElementById("breakdown").innerText =
      d.gp + " GPS | " + d.gl + " GLONASS | " + d.ga + " GALILEO | " + d.bd + " BEIDOU";

    document.getElementById("sats").innerText = d.sats;
    document.getElementById("snr").innerText = d.snr > 0 ? d.snr + " dB-Hz" : "--";
    document.getElementById("utc").innerText = d.utc;

    // Always display Latitude and Longitude
    document.getElementById("lat").innerText = Number(d.lat).toFixed(6);
    document.getElementById("lon").innerText = Number(d.lon).toFixed(6);

    const srcText = d.fix ? (d.simulated ? "Manual Test Pos" : "Live GPS Fix") : "Preset Coordinate";
    document.getElementById("latSource").innerText = srcText;
    document.getElementById("lonSource").innerText = srcText;

    document.getElementById("homeCoords").innerText =
      Number(d.homeLat).toFixed(6) + ", " + Number(d.homeLon).toFixed(6);

    document.getElementById("dist").innerText = Number(d.distance).toFixed(1) + " m";

    const status = document.getElementById("status");
    if (d.outside) {
      status.innerText = "OUTSIDE GEOFENCE";
      status.className = "value alert";
    } else {
      status.innerText = "SAFE";
      status.className = "value safe";
    }

    document.getElementById("alert").innerText = d.outside ? "ON" : "OFF";

    const hint = document.getElementById("hint");
    if (!d.dataOk && !d.simulated) {
      hint.style.display = "block";
      hint.innerText = "No live NMEA stream detected. Showing preset/manual coordinates (" +
                       Number(d.lat).toFixed(6) + ", " + Number(d.lon).toFixed(6) + ").";
    } else {
      hint.style.display = "none";
    }

    let position = [Number(d.lat), Number(d.lon)];

    document.getElementById("coordinates").innerText =
      "Current GPS: " + Number(d.lat).toFixed(6) + ", " + Number(d.lon).toFixed(6);

    document.getElementById("google").href =
      "https://www.google.com/maps?q=" + d.lat + "," + d.lon;

    if (mapAvailable) {
      if (!marker) {
        marker = L.marker(position).addTo(map).bindPopup("Current GPS Location");
      } else {
        marker.setLatLng(position);
      }

      if (!hasCentered) {
        map.setView(position, 18);
        hasCentered = true;
      }

      let home = [Number(d.homeLat), Number(d.homeLon)];
      if (!homeMarker) {
        homeMarker = L.marker(home).addTo(map).bindPopup("Home Location");
        fenceCircle = L.circle(home, {
          radius: d.fence,
          color: "red",
          fillColor: "#ff0000",
          fillOpacity: 0.12
        }).addTo(map);
      } else {
        homeMarker.setLatLng(home);
        fenceCircle.setLatLng(home);
        fenceCircle.setRadius(d.fence);
      }
    }
  } catch (error) {
    document.getElementById("link").innerText = "OFFLINE";
    document.getElementById("link").className = "value alert";
  }
}

updateDashboard();
setInterval(updateDashboard, 1000);
</script>
</body>
</html>
)rawliteral";

// =====================================================
// 11. DASHBOARD DATA & COORDINATE API
// =====================================================

void handleData() {
  bool fix = (gpsHasFix && (millis() - lastFixMillis < GPS_TIMEOUT)) || manualSimulatedFix;

  String json = "{";
  json += "\"fix\":";
  json += fix ? "true" : "false";

  json += ",\"simulated\":";
  json += manualSimulatedFix ? "true" : "false";

  json += ",\"dataOk\":";
  json += nmeaDataOk() ? "true" : "false";

  json += ",\"lat\":";
  json += String(currentLat, 6);

  json += ",\"lon\":";
  json += String(currentLon, 6);

  json += ",\"sats\":";
  json += String(satellites);

  json += ",\"inView\":";
  json += String(totalInView());

  json += ",\"tracked\":";
  json += String(totalTracked());

  json += ",\"snr\":";
  json += String(bestSnr());

  json += ",\"gp\":";
  json += String(satsInView[0]);

  json += ",\"gl\":";
  json += String(satsInView[1]);

  json += ",\"ga\":";
  json += String(satsInView[2]);

  json += ",\"bd\":";
  json += String(satsInView[3]);

  json += ",\"utc\":\"";
  json += utcTime;
  json += "\"";

  json += ",\"distance\":";
  json += String(currentDistance, 2);

  json += ",\"outside\":";
  json += outside ? "true" : "false";

  json += ",\"home\":";
  json += homeIsSet ? "true" : "false";

  json += ",\"homeLat\":";
  json += String(homeLat, 6);

  json += ",\"homeLon\":";
  json += String(homeLon, 6);

  json += ",\"homeCount\":";
  json += String(homeCount);

  json += ",\"homeSamples\":";
  json += String(HOME_SAMPLES);

  json += ",\"fence\":";
  json += String(FENCE_RADIUS_METERS, 1);

  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleSetCoords() {
  if (!server.hasArg("lat") || !server.hasArg("lon")) {
    server.send(400, "text/plain", "Missing lat or lon");
    return;
  }

  double lat = server.arg("lat").toDouble();
  double lon = server.arg("lon").toDouble();
  String mode = server.arg("mode");

  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    server.send(400, "text/plain", "Invalid coordinate range");
    return;
  }

  if (mode == "home") {
    homeLat = lat;
    homeLon = lon;
    homeIsSet = true;
    Serial.print("Home Fence Manually Updated -> Lat: ");
    Serial.print(homeLat, 6);
    Serial.print(", Lon: ");
    Serial.println(homeLon, 6);
  } else {
    currentLat = lat;
    currentLon = lon;
    manualSimulatedFix = true;
    Serial.print("Current Position Manually Updated -> Lat: ");
    Serial.print(currentLat, 6);
    Serial.print(", Lon: ");
    Serial.println(currentLon, 6);
  }

  evaluateGeofence();
  server.send(200, "text/plain", "OK");
}

// =====================================================
// 12. WIFI CONNECTION
// =====================================================

void connectWiFi() {
  Serial.println();
  Serial.println("================================");
  Serial.println("CONNECTING TO WIFI");
  Serial.println("================================");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    hotspotMode = false;
    Serial.println("WIFI CONNECTED SUCCESSFULLY!");
    Serial.print("IP ADDRESS: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("ROUTER WIFI CONNECTION FAILED!");
    Serial.println("STARTING ESP32 HOTSPOT...");

    WiFi.disconnect();
    WiFi.mode(WIFI_AP);

    bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);
    hotspotMode = apStarted;

    if (apStarted) {
      Serial.println("ESP32 HOTSPOT STARTED!");
      Serial.print("WIFI NAME: ");
      Serial.println(AP_SSID);
      Serial.print("WIFI PASSWORD: ");
      Serial.println(AP_PASSWORD);
    } else {
      Serial.println("HOTSPOT CREATION FAILED!");
    }
  }
}

// =====================================================
// 13. WEB SERVER ROUTES
// =====================================================

void startWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", webpage);
  });

  server.on("/data", HTTP_GET, handleData);
  server.on("/setcoords", HTTP_GET, handleSetCoords);

  server.on("/status", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 GPS Dashboard is running");
  });

  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204);
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Page not found");
  });

  server.begin();
  Serial.println("WEB SERVER STARTED!");
}

// =====================================================
// 14. PRINT DASHBOARD LINK
// =====================================================

void printDashboardLink() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("       GPS DASHBOARD IS READY");
  Serial.println("========================================");

  if (hotspotMode) {
    Serial.println("CONNECTION MODE: ESP32 HOTSPOT");
    Serial.print("WIFI NAME: ");
    Serial.println(AP_SSID);
    Serial.print("WIFI PASSWORD: ");
    Serial.println(AP_PASSWORD);
    Serial.println();
    Serial.println("CONNECT YOUR DEVICE TO ESP32 WIFI");
    Serial.println("DASHBOARD LINK: http://192.168.4.1");
  } else {
    Serial.println("CONNECTION MODE: ROUTER WIFI");
    Serial.print("DASHBOARD LINK: http://");
    Serial.println(WiFi.localIP());
  }

  Serial.println("========================================");
  Serial.println();
}

// =====================================================
// 15. SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32-C3 GPS GEOFENCING SYSTEM");
  Serial.println("================================");
  Serial.println("GPS TEST MODE: RAW NMEA OUTPUT ENABLED");
  Serial.println("Serial Monitor baud rate: 115200");

  gpsSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  pinMode(ALERT_PIN, OUTPUT);
  digitalWrite(ALERT_PIN, LOW);

  evaluateGeofence();
  connectWiFi();
  startWebServer();
  printDashboardLink();

  Serial.println("GPS SYSTEM READY");
  Serial.print("Initial Latitude:  ");
  Serial.println(currentLat, 6);
  Serial.print("Initial Longitude: ");
  Serial.println(currentLon, 6);
}

// =====================================================
// 16. MAIN LOOP
// =====================================================

void loop() {
  server.handleClient();

  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    bytesReceived++;

    if (c == '\n') {
      nmeaBuffer.trim();

      if (nmeaBuffer.length() > 0) {
        Serial.print("GPS RAW: ");
        Serial.println(nmeaBuffer);
        processGPS(nmeaBuffer);
      }

      nmeaBuffer = "";
    } else if (c != '\r') {
      nmeaBuffer += c;
      if (nmeaBuffer.length() > 120) {
        nmeaBuffer = "";
      }
    }

    server.handleClient();
  }

  if (gpsHasFix && millis() - lastFixMillis > GPS_TIMEOUT) {
    gpsHasFix = false;
    if (!manualSimulatedFix) {
      digitalWrite(ALERT_PIN, LOW);
    }
    Serial.println("GPS SIGNAL LOST!");
  }

  if (millis() - lastStatusMillis > 5000) {
    lastStatusMillis = millis();
    if (!gpsHasFix) {
      printStatus();
    }
  }
}