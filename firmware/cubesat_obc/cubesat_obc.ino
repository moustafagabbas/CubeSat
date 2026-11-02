/*
 * 1U CubeSat - On-Board Computer (OBC) firmware
 * Board: ESP32 Dev Module (Arduino framework)
 *
 * What it does:
 *   1. Reads all sensors every 2 seconds (no blocking delays).
 *   2. Prints a readable report on the Serial Monitor (115200 baud).
 *   3. Connects to Wi-Fi and runs a small web server:
 *        /      -> live sensor page (refreshes every 2 s)
 *        /data  -> latest values as CSV:
 *                  temp,humidity,gx,gy,gz,accX,accY,accZ,gas
 *
 * Design:
 *   - GPS on its own hardware UART (Serial2), separate from the USB port.
 *   - The loop never blocks; timing uses millis().
 *   - Gyroscope values in deg/s (the page shows deg/s).
 *   - Wi-Fi details in secrets.h (not uploaded to GitHub).
 *
 * Libraries (Arduino Library Manager):
 *   DHT sensor library (Adafruit), TinyGPSPlus, Adafruit MPU6050, Adafruit Unified Sensor
 *
 * Tip: GPIO 12, 13, 14 and 27 are ADC2 pins. If they read 0 while Wi-Fi is
 * on, read them before Wi-Fi starts or use ADC1 pins (GPIO 32-39).
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <TinyGPS++.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

#include "secrets.h"   // copy secrets.example.h to secrets.h first

// ---------------- Pin map (same as the PCBs) ----------------
#define DHT_PIN      25
#define DHT_TYPE     DHT11
#define GAS_PIN      12   // MQ-9, analog (ADC2)
#define UV_PIN       13   // GUVA-S12SD, analog (ADC2)
#define GPS_RX_PIN   16   // ESP32 RX2  <- GPS TX
#define GPS_TX_PIN   17   // ESP32 TX2  -> GPS RX
#define GPS_BAUD     9600

const int LDR_PINS[4]  = {32, 33, 27, 14};  // sun sensors, one per side
const int SIDE_PINS[4] = {36, 39, 34, 35};  // extra analog channels (ADCS sides)

const unsigned long READ_INTERVAL_MS = 2000;
const float RAD_TO_DEG_F = 57.2958f;

// ---------------- Objects ----------------
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
WebServer server(80);

// ---------------- Latest readings ----------------
struct SensorData {
  float temperature = NAN;
  float humidity    = NAN;
  float gx = 0, gy = 0, gz = 0;        // deg/s
  float accX = 0, accY = 0, accZ = 0;  // m/s^2
  int   gas = 0;
  int   uv  = 0;
  int   ldr[4]  = {0, 0, 0, 0};
  int   side[4] = {0, 0, 0, 0};
};

SensorData data;
bool mpuReady = false;
unsigned long lastRead = 0;

// ---------------- Ground station page ----------------
const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>CubeSat Sensor Data</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; }
    p    { color: #b00020; margin: 4px 0; }
  </style>
  <script>
    function updateData() {
      fetch('/data').then(r => r.text()).then(t => {
        const d = t.split(',');
        const ids = ['temperature','humidity','gx','gy','gz','accX','accY','accZ','gasValue'];
        ids.forEach((id, i) => document.getElementById(id).textContent = d[i]);
      });
    }
    setInterval(updateData, 2000);
  </script>
</head>
<body>
  <h1>CubeSat Sensor Data</h1>
  <p>Temperature: <span id="temperature">--</span> &deg;C</p>
  <p>Humidity: <span id="humidity">--</span> %</p>
  <h3>Rotation (&deg;/s)</h3>
  <p>gx: <span id="gx">--</span></p>
  <p>gy: <span id="gy">--</span></p>
  <p>gz: <span id="gz">--</span></p>
  <h3>Acceleration (m/s&sup2;)</h3>
  <p>X: <span id="accX">--</span></p>
  <p>Y: <span id="accY">--</span></p>
  <p>Z: <span id="accZ">--</span></p>
  <h3>Gas sensor (raw ADC)</h3>
  <p>MQ-9: <span id="gasValue">--</span></p>
</body>
</html>
)rawliteral";

// ---------------- Sensors ----------------
void readSensors() {
  data.temperature = dht.readTemperature();
  data.humidity    = dht.readHumidity();

  data.gas = analogRead(GAS_PIN);
  data.uv  = analogRead(UV_PIN);
  for (int i = 0; i < 4; i++) {
    data.ldr[i]  = analogRead(LDR_PINS[i]);
    data.side[i] = analogRead(SIDE_PINS[i]);
  }

  if (mpuReady) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    data.gx = g.gyro.x * RAD_TO_DEG_F;
    data.gy = g.gyro.y * RAD_TO_DEG_F;
    data.gz = g.gyro.z * RAD_TO_DEG_F;
    data.accX = a.acceleration.x;
    data.accY = a.acceleration.y;
    data.accZ = a.acceleration.z;
  }
}

void readGps() {
  // Feed every byte from the GPS into the parser. Never blocks.
  while (Serial2.available() > 0) {
    gps.encode(Serial2.read());
  }
}

// ---------------- Serial report ----------------
void printReport() {
  Serial.println(F("------------- CubeSat report -------------"));
  Serial.printf("Temperature: %.1f C   Humidity: %.1f %%\n", data.temperature, data.humidity);
  Serial.printf("Gas (MQ-9): %d   UV (GUVA): %d\n", data.gas, data.uv);
  Serial.printf("Gyro  [deg/s]: x=%.2f y=%.2f z=%.2f\n", data.gx, data.gy, data.gz);
  Serial.printf("Accel [m/s2] : x=%.2f y=%.2f z=%.2f\n", data.accX, data.accY, data.accZ);
  Serial.printf("Sun sensors (LDR 1-4): %d %d %d %d\n",
                data.ldr[0], data.ldr[1], data.ldr[2], data.ldr[3]);
  Serial.printf("Side ADC (1-4): %d %d %d %d\n",
                data.side[0], data.side[1], data.side[2], data.side[3]);

  if (gps.location.isValid()) {
    Serial.printf("GPS: sats=%u lat=%.6f lng=%.6f alt=%.1f m speed=%.1f km/h\n",
                  (unsigned)gps.satellites.value(), gps.location.lat(), gps.location.lng(),
                  gps.altitude.meters(), gps.speed.kmph());
  } else if (gps.charsProcessed() < 10) {
    Serial.println(F("GPS: no data (check wiring)"));
  } else {
    Serial.println(F("GPS: waiting for a fix"));
  }

  if (gps.date.isValid() && gps.time.isValid()) {
    Serial.printf("UTC: %02d/%02d/%04d %02d:%02d:%02d\n",
                  gps.date.day(), gps.date.month(), gps.date.year(),
                  gps.time.hour(), gps.time.minute(), gps.time.second());
  }
  Serial.printf("Wi-Fi: %s\n\n",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
}

// ---------------- Web server ----------------
void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

void handleData() {
  // Order must match the page: temp,humidity,gx,gy,gz,accX,accY,accZ,gas
  String csv;
  csv.reserve(96);
  csv += String(data.temperature, 1) + ",";
  csv += String(data.humidity, 1) + ",";
  csv += String(data.gx, 2) + ",";
  csv += String(data.gy, 2) + ",";
  csv += String(data.gz, 2) + ",";
  csv += String(data.accX, 2) + ",";
  csv += String(data.accY, 2) + ",";
  csv += String(data.accZ, 2) + ",";
  csv += String(data.gas);
  server.send(200, "text/plain", csv);
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("Connecting to Wi-Fi"));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Open this page: http://"));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("Wi-Fi not connected. Serial output still works."));
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  analogReadResolution(12);   // 0..4095
  for (int i = 0; i < 4; i++) {
    pinMode(LDR_PINS[i], INPUT);
    pinMode(SIDE_PINS[i], INPUT);
  }

  dht.begin();
  Wire.begin();

  mpuReady = mpu.begin();
  if (mpuReady) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_5_HZ);
    Serial.println(F("MPU6050 found"));
  } else {
    Serial.println(F("MPU6050 not found, check wiring. Continuing without it."));
  }

  startWifi();
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  readGps();              // keep the GPS parser fed
  server.handleClient();  // answer web requests

  if (millis() - lastRead >= READ_INTERVAL_MS) {
    lastRead = millis();
    readSensors();
    printReport();
  }
}
