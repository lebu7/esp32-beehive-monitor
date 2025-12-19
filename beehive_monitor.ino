/*
  ESP32 Multi-sensor monitoring with web portal
  Sensors/Modules:
   - DHT22 (temperature + humidity)
   - HX711 (load cell)
   - MQ135 (air quality, analog)
   - I2C LCD (LiquidCrystal_I2C)
   - SIM800 (GSM) via Serial2 (for SMS)
   - Web portal served from ESP32 (Async Web Server)
  
  -------------------------------------------------------------------------
  REQUIRED LIBRARIES (Download from GitHub or Library Manager):
  -------------------------------------------------------------------------
   1. DHT Sensor Library: https://github.com/adafruit/DHT-sensor-library
      (+ Adafruit Unified Sensor: https://github.com/adafruit/Adafruit_Sensor)
   2. HX711 (Bogde):      https://github.com/bogde/HX711
   3. LiquidCrystal_I2C:  https://github.com/johnrickman/LiquidCrystal_I2C
   4. SQLite3 for ESP32:  https://github.com/siara-cc/esp32_arduino_sqlite3_lib
  -------------------------------------------------------------------------

  Notes:
   - Install these Arduino libraries:
       ESPAsyncWebServer, AsyncTCP, DHT sensor library, HX711 by Bogde, LiquidCrystal_I2C
   - Configure WIFI_SSID and WIFI_PASS below
   - Adjust pin assignments for your wiring
   - This is a starter, production usage requires calibrations (HX711), MQ135 calibration, and robust SIM800 handlingDefault Login:
   - User: admin
   - Pass: admin
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include "HX711.h"
#include <LiquidCrystal_I2C.h>
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <FS.h>
#include <LittleFS.h>
#include "time.h"

// -------------------- USER CONFIG --------------------
const char* WIFI_SSID = "Wifi Name";
const char* WIFI_PASS = "Wifi password";

// --- LOGIN CREDENTIALS ---
const char* login_user = "admin";
const char* login_pass = "admin";

// Time settings
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10800; 
const int   daylightOffset_sec = 0;

// SMS recipient (Default fallback)
const char* SMS_RECIPIENT = "+1234567890";

// Pins
#define DHTPIN 4
#define DHTTYPE DHT22
#define MQ135_PIN 35
#define HX711_DOUT 33
#define HX711_SCK 32

// I2C LCD
#define LCD_I2C_ADDR 0x3F
#define LCD_COLS 16
#define LCD_ROWS 2

// SIM800
#define SIM800_RX_PIN 16
#define SIM800_TX_PIN 17
#define SIM800_BAUD 9600

// Database Settings
const char* dbPath = "/littlefs/sensor.db";
sqlite3 *db1;
const unsigned long DB_SAVE_INTERVAL = 30000; 

// -------------------- GLOBALS --------------------
DHT dht(DHTPIN, DHTTYPE);
HX711 scale;
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
WebServer server(80); 
HardwareSerial SIM800(2);

// Sensor variables
float temperature = 0.0;
float humidity = 0.0;
float weight = 0.0;
int mq135_raw = 0;

// Settings Variables
String activePhoneNumber = "+254700000000"; 
float lim_temp = 40.0;
float lim_hum = 85.0;
float lim_weight = 50.0;
int lim_air = 400;

// Session Management
unsigned long session_expiry = 0;
const unsigned long SESSION_TIMEOUT = 1800000; 
bool is_logged_in = false;

// Timers
unsigned long lastSensorMillis = 0;
unsigned long lastDbMillis = 0;
unsigned long lastWifiCheck = 0; 
const unsigned long SENSOR_INTERVAL = 2000; 
const unsigned long SMS_COOLDOWN = 300000; 

// SMS State Machine
enum SmsState { SMS_IDLE, SMS_MODE, SMS_NUMBER, SMS_CONTENT, SMS_WAIT };
SmsState currentSmsState = SMS_IDLE;
unsigned long smsTimer = 0;
unsigned long lastSmsCooldown = 0;
String smsQueueNum = "";
String smsQueueMsg = "";

// -------------------- HELPER FUNCTIONS --------------------

void initTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)) Serial.println("Time Synced");
}

String getTimestamp() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "N/A";
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// SECURE AUTHENTICATION
bool is_authenticated() {
  if (!is_logged_in) return false;
  if (millis() > session_expiry) {
    is_logged_in = false; 
    return false;
  }
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    if (cookie.indexOf("ESPSESSIONID=1") != -1) {
      session_expiry = millis() + SESSION_TIMEOUT; 
      return true;
    }
  }
  return false;
}

static int callback(void *data, int argc, char **argv, char **azColName){ return 0; }

// -------------------- DATABASE --------------------

void openDatabase() {
    if (sqlite3_open(dbPath, &db1)){ Serial.println("DB Open Fail"); return; }
    
    // CRITICAL
    sqlite3_exec(db1, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(db1, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db1, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);

    char *zErrMsg = 0;
    sqlite3_exec(db1, "CREATE TABLE IF NOT EXISTS readings (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, temp REAL, hum REAL, weight REAL, mq INTEGER);", callback, 0, &zErrMsg);
    sqlite3_exec(db1, "CREATE TABLE IF NOT EXISTS settings (id INTEGER PRIMARY KEY, phone TEXT, lim_t REAL, lim_h REAL, lim_w REAL, lim_a INTEGER);", callback, 0, &zErrMsg);
    sqlite3_exec(db1, "INSERT OR IGNORE INTO settings (id, phone, lim_t, lim_h, lim_w, lim_a) VALUES (1, '+254700000000', 40.0, 85.0, 50.0, 400);", callback, 0, &zErrMsg);
}

void loadSettings() {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db1, "SELECT phone, lim_t, lim_h, lim_w, lim_a FROM settings WHERE id=1;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* ph = (const char*)sqlite3_column_text(stmt, 0);
            if(ph) activePhoneNumber = String(ph);
            lim_temp = sqlite3_column_double(stmt, 1);
            lim_hum = sqlite3_column_double(stmt, 2);
            lim_weight = sqlite3_column_double(stmt, 3);
            lim_air = sqlite3_column_int(stmt, 4);
        }
    }
    sqlite3_finalize(stmt);
}

// OPTIMIZED SAVE 
void saveToDatabase() {
    String ts = getTimestamp();
    if(ts == "N/A") { 
       unsigned long seconds = millis() / 1000;
       char buff[20]; sprintf(buff, "Offline-%02lu", seconds); ts = String(buff);
    }

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO readings (timestamp, temp, hum, weight, mq) VALUES (?, ?, ?, ?, ?);";
    
    if (sqlite3_prepare_v2(db1, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, temperature);
        sqlite3_bind_double(stmt, 3, humidity);
        sqlite3_bind_double(stmt, 4, weight);
        sqlite3_bind_int(stmt, 5, mq135_raw);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            Serial.println("Saved: " + ts);
        }
        sqlite3_finalize(stmt);
    }
}

// -------------------- SMS --------------------

void initSIM800() {
  SIM800.begin(SIM800_BAUD, SERIAL_8N1, SIM800_RX_PIN, SIM800_TX_PIN);
  delay(100); 
  SIM800.println("AT");
}

void triggerSMS(String number, String message) {
  if (currentSmsState == SMS_IDLE) {
    smsQueueNum = number;
    smsQueueMsg = message;
    currentSmsState = SMS_MODE;
    Serial.println("SMS Queued");
  }
}

void manageSMS() {
  if (currentSmsState == SMS_IDLE) return;
  unsigned long now = millis();
  
  switch (currentSmsState) {
    case SMS_MODE:
      SIM800.println("AT+CMGF=1");
      smsTimer = now;
      currentSmsState = SMS_NUMBER;
      break;
    case SMS_NUMBER:
      if (now - smsTimer > 200) {
        SIM800.print("AT+CMGS=\""); SIM800.print(smsQueueNum); SIM800.println("\"");
        smsTimer = now;
        currentSmsState = SMS_CONTENT;
      }
      break;
    case SMS_CONTENT:
      if (now - smsTimer > 200) {
        SIM800.print(smsQueueMsg);
        SIM800.write(26);
        smsTimer = now;
        currentSmsState = SMS_WAIT;
      }
      break;
    case SMS_WAIT:
      if (now - smsTimer > 3000) { 
        currentSmsState = SMS_IDLE; 
        Serial.println("SMS Processed");
      }
      break;
  }
}

void checkAlerts() {
  if (millis() - lastSmsCooldown < SMS_COOLDOWN) return;
  String msg = "";
  bool alert = false;

  if (temperature > lim_temp) {
    msg = "⚠️ Alert: High Temperature! Reading: " + String(temperature, 1) + "C. Limit: " + String(lim_temp, 1);
    alert = true;
  }
  else if (humidity > lim_hum) {
    msg = "⚠️ Alert: High Humidity! Reading: " + String(humidity, 1) + "%. Limit: " + String(lim_hum, 1);
    alert = true;
  }
  else if (weight > lim_weight) {
    msg = "⚠️ Alert: Weight Exceeded! Reading: " + String(weight, 1) + "kg. Limit: " + String(lim_weight, 1);
    alert = true;
  }
  else if (mq135_raw > lim_air) {
    msg = "⚠️ Alert: Poor Air Quality! Reading: " + String(mq135_raw) + ". Limit: " + String(lim_air);
    alert = true;
  }

  if (alert) {
    triggerSMS(activePhoneNumber, msg);
    lastSmsCooldown = millis();
  }
}

// -------------------- HTML PAGES --------------------

// --- LOGIN PAGE ---
const char loginPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Lebu's Beehive Farm Login</title>
  <style>
    body { font-family: Arial, sans-serif; display: flex; justify-content: center; align-items: center; height: 100vh; background: #f0f2f5; margin: 0; }
    .login-card { background: white; padding: 2rem; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); width: 300px; text-align: center; }
    h2 { margin-bottom: 1.5rem; color: #333; display: flex; align-items: center; justify-content: center; gap: 10px; }
    .hive-icon { width: 80px; height: 80px; margin: 0 auto 15px auto; display: block; }
    .bee-icon { width: 32px; height: 32px; }
    input { width: 100%; padding: 10px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; }
    /* YELLOW BUTTON */
    button { width: 100%; padding: 10px; background: #fbbf24; color: #000; border: none; border-radius: 4px; cursor: pointer; font-size: 1rem; font-weight: bold; }
    button:hover { background: #f59e0b; }
  </style>
</head>
<body>
  <div class="login-card">
    <img src="https://www.svgrepo.com/show/295920/honeycomb.svg" alt="Honeycomb Icon" class="hive-icon">
    <h2><img src="https://www.svgrepo.com/show/299032/bee.svg" alt="Bee Icon" class="bee-icon"> Lebu's Beehive Farm</h2>
    <form action="/login" method="POST">
      <input type="text" name="username" placeholder="Username" required>
      <input type="password" name="password" placeholder="Password" required>
      <button type="submit">Login</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

// --- DASHBOARD ---
const char dashboardPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Lebu's Farm Dashboard</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 16px; background: #f4f4f9; }
    .container { max-width: 900px; margin: auto; }
    .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
    .header h2 { margin: 0; color: #333; display: flex; align-items: center; gap: 10px;}
    .card { background: white; border-radius: 8px; padding: 15px; margin-bottom: 15px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
    .title { font-weight: bold; font-size: 1.1em; color: #333; margin-bottom: 5px; }
    .val { font-size: 1.5em; color: #007bff; }
    
    .bee-icon { width: 36px; height: 36px; }
    .hive-icon-small { width: 20px; height: 20px; margin-right: 5px; }
    .live-icon { width: 20px; height: 20px; margin-right: 5px; }
    .edit-icon { width: 24px; height: 24px; }
    
    .controls-grid { display: flex; flex-wrap: wrap; gap: 8px; align-items: flex-end; }
    .input-wrap { flex: 1; min-width: 60px; }
    .input-wrap.phone { flex: 2; min-width: 140px; }
    .input-wrap label { font-size: 0.75em; color: #555; display: block; margin-bottom: 2px;}
    
    .input-wrap input { width: 100%; padding: 8px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; color: black; }
    .input-wrap input:disabled { background-color: #f0f0f0; color: #888; border: 1px solid #ddd; cursor: not-allowed; }

    .action-group { display: flex; gap: 5px; align-items: center; margin-bottom: 2px;}
    .btn-edit { background: none; border: none; font-size: 1.5em; cursor: pointer; color: #555; padding: 0 10px;}
    .btn-save { background: #28a745; color: white; border: none; padding: 0 20px; height: 35px; border-radius: 4px; cursor: pointer; display: none; }
    .btn-test { background: #ffc107; color: black; font-size:0.8em; padding: 5px 10px; border:none; border-radius:4px; cursor:pointer; margin-top:10px; float: right;}

    table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
    th { background-color: #007bff; color: white; }
    .btn { background: #6f42c1; color: white; text-decoration: none; padding: 8px 15px; border-radius: 5px; display: inline-block;}
    .btn-logout { background: #dc3545; font-size: 0.9rem; padding: 8px 15px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
        <h2><img src="https://www.svgrepo.com/show/299032/bee.svg" alt="Bee Icon" class="bee-icon"> Lebu's Beehive Dashboard</h2>
        <a href="/logout" class="btn btn-logout">Logout</a>
    </div>
    
    <div style="display: flex; flex-wrap: wrap; gap: 10px;">
      <div class="card" style="flex: 1;"><div class="title">Temperature</div><div id="temp" class="val">-- °C</div></div>
      <div class="card" style="flex: 1;"><div class="title">Humidity</div><div id="hum" class="val">-- %</div></div>
      <div class="card" style="flex: 1;"><div class="title">Weight</div><div id="weight" class="val">--</div></div>
      <div class="card" style="flex: 1;"><div class="title">Air Quality</div><div id="mq" class="val">--</div></div>
    </div>

    <div class="card">
      <div class="title">Alert Settings & Limits</div>
      <form action="/save_settings" method="POST" class="controls-grid" id="settingsForm">
         <div class="input-wrap phone">
            <label>Phone Number</label>
            <input type="text" name="phone" id="phone" placeholder="+254..." disabled>
         </div>
         <div class="input-wrap">
            <label>Max Temp</label>
            <input type="text" name="lt" id="lt" disabled>
         </div>
         <div class="input-wrap">
            <label>Max Hum</label>
            <input type="text" name="lh" id="lh" disabled>
         </div>
         <div class="input-wrap">
            <label>Max Kg</label>
            <input type="text" name="lw" id="lw" disabled>
         </div>
         <div class="input-wrap">
            <label>Max Air</label>
            <input type="text" name="la" id="la" disabled>
         </div>
         
         <div class="action-group">
             <button type="button" class="btn-edit" onclick="toggleEdit()" title="Edit Settings">
                <img src="https://www.svgrepo.com/show/522527/edit-3.svg" alt="Edit" class="edit-icon">
             </button>
             <button type="submit" class="btn-save" id="saveBtn">Save</button>
         </div>
      </form>
      <button class="btn-test" onclick="sendTestSMS()">Send Test SMS</button>
      <div style="clear:both"></div>
    </div>

    <div class="card">
      <div style="display:flex; justify-content:space-between; align-items:center;">
          <div class="title"><img src="https://www.svgrepo.com/show/486720/live-schedule.svg" alt="Live Icon" class="live-icon"> Live History (Updates 30s)</div>
          <a href="/report" class="btn btn-report">View Monthly Report</a>
      </div>
      <table id="histTable">
        <thead><tr><th>Time</th><th>Temp</th><th>Hum</th><th>Wt</th><th>Air</th></tr></thead>
        <tbody><tr><td colspan="5">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>

<script>
async function update(){
  try{
    const r = await fetch('/data');
    if(r.redirected) window.location.href = r.url; 
    const j = await r.json();
    document.getElementById('temp').innerText = j.temp.toFixed(2) + ' °C';
    document.getElementById('hum').innerText = j.hum.toFixed(2) + ' %';
    document.getElementById('weight').innerText = j.weight.toFixed(2);
    document.getElementById('mq').innerText = j.air;
  } catch(e){}
}

async function loadSettings(){
  try{
    const r = await fetch('/get_settings');
    const j = await r.json();
    document.getElementById('phone').value = j.phone;
    document.getElementById('lt').value = j.lt;
    document.getElementById('lh').value = j.lh;
    document.getElementById('lw').value = j.lw;
    document.getElementById('la').value = j.la;
  } catch(e){}
}

function toggleEdit() {
    const inputs = document.querySelectorAll('#settingsForm input');
    document.getElementById('saveBtn').style.display = 'block'; 
    document.querySelector('.btn-edit').style.display = 'none'; 
    inputs.forEach(input => input.disabled = false);
}

async function sendTestSMS(){
  if(confirm("Send test SMS?")){
    const resp = await fetch('/sms', {method:'POST'});
    if(resp.redirected) window.location.href = resp.url;
    alert(await resp.text());
  }
}

async function loadHistory(){
  try {
    const r = await fetch('/history');
    if(r.redirected) window.location.href = r.url;
    const data = await r.json();
    const tbody = document.querySelector('#histTable tbody');
    tbody.innerHTML = '';
    data.forEach(row => {
      const tr = document.createElement('tr');
      tr.innerHTML = `<td>${row.timestamp}</td><td>${row.temp}</td><td>${row.hum}</td><td>${row.weight}</td><td>${row.mq}</td>`;
      tbody.appendChild(tr);
    });
  } catch(e) {}
}

setInterval(update, 2000); 
setInterval(loadHistory, 30000); 
update(); loadHistory(); loadSettings();
</script>
</body>
</html>
)rawliteral";

// --- REPORT PAGE:---
const char reportPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Monthly Report - Lebu's Beehive Farm</title>
  <style>
    body { font-family: 'Segoe UI', Arial, sans-serif; padding: 20px; background: #f4f4f9; color: #333; }
    .container { max-width: 950px; margin: auto; background: white; padding: 25px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.05); }
    .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 25px; border-bottom: 2px solid #eee; padding-bottom: 15px; }
    .header h2 { margin: 0; color: #2c3e50; display: flex; align-items: center; gap: 10px; }
    
    .bee-icon-report { width: 40px; height: 40px; }
    .print-icon, .download-icon { width: 20px; height: 20px; margin-right: 8px; }
    .footer-icon { width: 18px; height: 18px; margin-right: 5px; }
    
    .btn-group { display: flex; gap: 10px; }
    .btn { background: #555; color: white; text-decoration: none; padding: 10px 18px; border-radius: 5px; border: none; cursor: pointer; display: inline-flex; align-items: center; gap: 8px; font-size: 14px; font-weight: 500; transition: background 0.2s;}
    .btn:hover { background: #333; }
    .btn-print { background: #007bff; }
    .btn-print:hover { background: #0056b3; }
    .btn-download { background: #28a745; }
    .btn-download:hover { background: #218838; }

    .table-responsive { overflow-x: auto; -webkit-overflow-scrolling: touch; margin-top: 10px; border: 1px solid #e1e4e8; border-radius: 4px;}
    table { width: 100%; min-width: 600px; border-collapse: collapse; background-color: white; }
    
    th, td { padding: 12px 15px; text-align: left; white-space: nowrap; border-bottom: 1px solid #e1e4e8; }
    th { background-color: #6f42c1; color: white; font-weight: 600; text-transform: uppercase; font-size: 0.85rem; letter-spacing: 0.5px; position: sticky; top: 0;}
    tbody tr:nth-child(even) { background-color: #f8f9fa; }
    tbody tr:hover { background-color: #e9ecef; }
    
    .footer { margin-top: 25px; padding-top: 15px; border-top: 1px solid #eee; color: #7f8c8d; font-size: 13px; display: flex; justify-content: space-between; flex-wrap: wrap; gap: 10px;}
    
    /* PRINTING FIX: Forces Background Colors & Images */
    @media print {
      body { background: white; padding: 0; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
      .container { box-shadow: none; max-width: 100%; padding: 0; margin: 0;}
      .btn-group { display: none; }
      .header { border-bottom: 2px solid #333; }
      
      /* Ensure icons show up */
      img { display: inline-block !important; visibility: visible !important; }
      
      .table-responsive { border: none; overflow: visible; }
      table { page-break-inside: avoid; border: 1px solid #000; min-width: 100%;}
      th, td { border: 1px solid #000; color: #000; white-space: normal;}
      
      /* Force Purple Header Color */
      th { background-color: #6f42c1 !important; color: white !important; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
      
      thead { display: table-header-group; }
      .footer { color: #000; border-top: 1px solid #000; }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h2><img src="https://www.svgrepo.com/show/212068/hive.svg" alt="Hive Icon" class="bee-icon-report"> 30-Day Daily Averages</h2>
      <div class="btn-group">
        <button onclick="exportCSV()" class="btn btn-download">
          <img src="https://www.svgrepo.com/show/526531/download-minimalistic.svg" alt="Download Icon" class="download-icon">
          Download CSV
        </button>
        <button onclick="window.print()" class="btn btn-print">
          <img src="https://www.svgrepo.com/show/451207/print.svg" alt="Print Icon" class="print-icon">
          Print Report
        </button>
        <a href="/dashboard" class="btn">Back</a>
      </div>
    </div>
    
    <div style="color: #666; margin-bottom: 15px; font-size: 14px;">
      <strong>Report Generated:</strong> <span id="reportDate"></span>
    </div>
    
    <div class="table-responsive">
      <table id="reportTable">
        <thead>
          <tr><th>Date</th><th>Avg Temp (°C)</th><th>Avg Humidity (%)</th><th>Avg Weight (kg)</th><th>Avg Air Quality</th></tr>
        </thead>
        <tbody><tr><td colspan="5" style="text-align:center; padding: 20px;">Loading data...</td></tr></tbody>
      </table>
    </div>
    
    <div class="footer">
      <span><img src="https://www.svgrepo.com/show/299032/bee.svg" alt="Bee Icon" class="footer-icon"> <strong>Lebu's Beehive Farm</strong> - Automated Monitoring</span>
      <span>*Data excludes offline readings</span>
    </div>
  </div>
  
  <script>
    document.getElementById('reportDate').innerText = new Date().toLocaleString();
    let reportData = []; 

    fetch('/monthly_data')
      .then(r => { if(r.redirected) window.location.href = r.url; return r.json(); })
      .then(data => {
        reportData = data; 
        const tbody = document.querySelector('#reportTable tbody');
        tbody.innerHTML = '';
        if(data.length === 0) { tbody.innerHTML = '<tr><td colspan="5" style="text-align:center;">No data available</td></tr>'; return; }
        data.forEach(row => {
          tbody.innerHTML += `<tr><td>${row.day}</td><td>${row.avg_temp.toFixed(2)}</td><td>${row.avg_hum.toFixed(2)}</td><td>${row.avg_weight.toFixed(2)}</td><td>${row.avg_mq.toFixed(0)}</td></tr>`;
        });
      })
      .catch(err => { document.querySelector('#reportTable tbody').innerHTML = '<tr><td colspan="5" style="text-align:center; color:red;">Error loading data</td></tr>'; });

    function exportCSV() {
      if(!reportData.length) { alert("No data to export"); return; }
      let csv = 'Date,Avg Temperature,Avg Humidity,Avg Weight,Avg Air Quality\n';
      reportData.forEach(row => {
        csv += `${row.day},${row.avg_temp.toFixed(2)},${row.avg_hum.toFixed(2)},${row.avg_weight.toFixed(2)},${row.avg_mq.toFixed(0)}\n`;
      });
      
      const blob = new Blob([csv], { type: 'text/csv' });
      const url = window.URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `Lebu's-report-${new Date().toISOString().split('T')[0]}.csv`;
      a.click();
      window.URL.revokeObjectURL(url);
    }
  </script>
</body>
</html>
)rawliteral";

// -------------------- SETUP & LOOP --------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  if(!LittleFS.begin(true)){ Serial.println("FS Fail"); return; }

  // Startup Diagnostics
  Serial.println("=== DIAGNOSTICS ===");
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.println("==================");

  dht.begin();
  scale.begin(HX711_DOUT, HX711_SCK);
  
  lcd.init(); lcd.backlight();
  lcd.setCursor(0,0); lcd.print("Welcome to Lebu's");
  lcd.setCursor(0,1); lcd.print("  Beehive Farm"); 
  delay(5000); 
  lcd.setCursor(0,0); lcd.print("   System Boot  ");
  lcd.setCursor(0,1); lcd.print("     ......     "); 

  scale.set_scale(); scale.tare();     

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  Serial.print("Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(500); Serial.print('.');
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected IP: " + WiFi.localIP().toString());
    lcd.clear(); 
    lcd.setCursor(0,0); lcd.print("IP: "); lcd.print(WiFi.localIP()); 
    lcd.setCursor(0,1); lcd.print("Net: "); lcd.print(WIFI_SSID); 
    delay(5000);
    initTime();
  } else {
    Serial.println("WiFi Fail");
    lcd.clear(); lcd.print("WiFi Fail");
  }

  sqlite3_initialize();
  openDatabase();
  loadSettings(); 
  initSIM800();
  
  saveToDatabase(); 

 // --- ROUTES ---
  const char * headerkeys[] = {"Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.on("/", HTTP_GET, []() {
    if (is_authenticated()) { server.sendHeader("Location", "/dashboard"); server.send(303); return; }
    server.send(200, "text/html", loginPage);
  });

  server.on("/login", HTTP_POST, []() {
    if (server.hasArg("username") && server.hasArg("password")) {
      if (server.arg("username") == login_user && server.arg("password") == login_pass) {
        server.sendHeader("Location", "/dashboard");
        server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
        is_logged_in = true; 
        session_expiry = millis() + SESSION_TIMEOUT;
        server.send(303);
        return;
      }
    }
    server.send(401, "text/html", "Login Failed. <a href='/'>Try again</a>");
  });

  server.on("/logout", []() {
    server.sendHeader("Location", "/");
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0; Max-Age=0");
    is_logged_in = false;
    server.send(303);
  });

  // Optimized Cache Control
  server.on("/dashboard", []() {
    if (!is_authenticated()) { server.sendHeader("Location", "/"); server.send(303); return; }
    server.sendHeader("Cache-Control", "max-age=60");
    server.send(200, "text/html", dashboardPage);
  });

  server.on("/report", []() {
    if (!is_authenticated()) { server.sendHeader("Location", "/"); server.send(303); return; }
    server.sendHeader("Cache-Control", "max-age=60");
    server.send(200, "text/html", reportPage);
  });

  // Optimized JSON API
  server.on("/data", [](){
    if (!is_authenticated()) return server.send(303, "text/plain", "/");
    
    char json[256];
    snprintf(json, sizeof(json), 
      "{\"temp\":%.2f,\"hum\":%.2f,\"weight\":%.2f,\"air\":%d}",
      temperature, humidity, weight, mq135_raw);
    
    server.send(200, "application/json", json);
  });

  server.on("/get_settings", [](){
    if (!is_authenticated()) return server.send(303, "text/plain", "/");
    
    char json[256];
    snprintf(json, sizeof(json), 
      "{\"phone\":\"%s\",\"lt\":%.1f,\"lh\":%.1f,\"lw\":%.1f,\"la\":%d}",
      activePhoneNumber.c_str(), lim_temp, lim_hum, lim_weight, lim_air);
      
    server.send(200, "application/json", json);
  });

  server.on("/save_settings", HTTP_POST, [](){
    if (!is_authenticated()) return server.send(303, "text/plain", "/");
    if(server.hasArg("phone")){
       // 1. UPDATE MEMORY
       activePhoneNumber = server.arg("phone");
       lim_temp = server.arg("lt").toFloat();
       lim_hum = server.arg("lh").toFloat();
       lim_weight = server.arg("lw").toFloat();
       lim_air = server.arg("la").toInt();
       
       // 2. SEND RESPONSE FIRST (Prevent Timeout)
       server.sendHeader("Location", "/dashboard");
       server.send(303);
       
       // 3. WRITE TO DB 
       sqlite3_stmt *stmt;
       const char* sql = "UPDATE settings SET phone=?, lim_t=?, lim_h=?, lim_w=?, lim_a=? WHERE id=1;";
       if (sqlite3_prepare_v2(db1, sql, -1, &stmt, NULL) == SQLITE_OK) {
           sqlite3_bind_text(stmt, 1, activePhoneNumber.c_str(), -1, SQLITE_TRANSIENT);
           sqlite3_bind_double(stmt, 2, lim_temp);
           sqlite3_bind_double(stmt, 3, lim_hum);
           sqlite3_bind_double(stmt, 4, lim_weight);
           sqlite3_bind_int(stmt, 5, lim_air);
           sqlite3_step(stmt);
           sqlite3_finalize(stmt);
           Serial.println("Settings Saved to DB");
       }
    } else {
       server.send(400, "text/plain", "Bad Request");
    }
  });

  server.on("/sms", HTTP_POST, [](){
    if (!is_authenticated()) return server.send(303, "text/plain", "/");
    triggerSMS(activePhoneNumber, "Test Alert from Lebu's Farm System.");
    server.send(200, "text/plain", "Queued");
  });

  // Optimized History 
  server.on("/history", [](){
    if (!is_authenticated()) return server.send(303, "text/plain", "/");
    
    String json;
    json.reserve(1200); 
    json = "[";
    
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db1, "SELECT timestamp, temp, hum, weight, mq FROM readings ORDER BY id DESC LIMIT 10;", -1, &stmt, NULL) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json += ",";
            
            char row[150];
            snprintf(row, sizeof(row), 
              "{\"timestamp\":\"%s\",\"temp\":%.2f,\"hum\":%.2f,\"weight\":%.2f,\"mq\":%d}",
              sqlite3_column_text(stmt, 0),
              sqlite3_column_double(stmt, 1),
              sqlite3_column_double(stmt, 2),
              sqlite3_column_double(stmt, 3),
              sqlite3_column_int(stmt, 4));
            
            json += row;
            first = false;
        }
    }
    sqlite3_finalize(stmt);
    json += "]";
    server.send(200, "application/json", json);
  });

  // Monthly Data 
  server.on("/monthly_data", [](){
    if (!is_authenticated()) return server.send(303, "text/plain", "/");
    
    String json;
    json.reserve(2000); 
    json = "[";
    
    sqlite3_stmt *stmt;
    const char *sql = "SELECT substr(timestamp, 1, 10) as day, AVG(temp), AVG(hum), AVG(weight), AVG(mq) FROM readings WHERE timestamp NOT LIKE 'Offline-%' GROUP BY day ORDER BY day DESC LIMIT 30;";
    if (sqlite3_prepare_v2(db1, sql, -1, &stmt, NULL) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json += ",";
            
            char row[150];
            snprintf(row, sizeof(row), 
              "{\"day\":\"%s\",\"avg_temp\":%.2f,\"avg_hum\":%.2f,\"avg_weight\":%.2f,\"avg_mq\":%d}",
              sqlite3_column_text(stmt, 0),
              sqlite3_column_double(stmt, 1),
              sqlite3_column_double(stmt, 2),
              sqlite3_column_double(stmt, 3),
              sqlite3_column_int(stmt, 4));
            
            json += row;
            first = false;
        }
    }
    sqlite3_finalize(stmt);
    json += "]";
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("Web Server Started");
}

void loop() {
  server.handleClient();
  manageSMS();
  
  unsigned long now = millis();

  // Optimized WiFi Watchdog
  if (now - lastWifiCheck > 10000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi Lost - Reconnecting...");
      WiFi.reconnect();
    }
  }

  // Sensors & LCD
  if (now - lastSensorMillis >= SENSOR_INTERVAL) {
    lastSensorMillis = now;
    
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;
    if (!isnan(h)) humidity = h;
    if (scale.is_ready()) weight = scale.get_units(10);
    mq135_raw = analogRead(MQ135_PIN);

    lcd.setCursor(0,0); lcd.printf("Tem:%-4.1f ", temperature); 
    lcd.setCursor(9,0); lcd.printf("Hum:%2.0f%%", humidity);
    lcd.setCursor(0,1); lcd.printf("Kg:%-5.1f ", weight);
    lcd.setCursor(9,1); lcd.printf("Air:%3d", mq135_raw);

    checkAlerts();
  }

  // Database Save 
  if (now - lastDbMillis >= DB_SAVE_INTERVAL) {
    if(currentSmsState == SMS_IDLE) { 
        lastDbMillis = now;
        saveToDatabase();
    }
  }
}