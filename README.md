# 🐝 ESP32 Beehive Monitoring System

A comprehensive IoT solution for real-time beehive monitoring with web dashboard, SMS alerts, and data logging capabilities.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-green.svg)
![Status](https://img.shields.io/badge/status-active-success.svg)

## 📋 Table of Contents

- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [Software Requirements](#software-requirements)
- [Installation](#installation)
- [Wiring Diagram](#wiring-diagram)
- [Configuration](#configuration)
- [Usage](#usage)
- [Web Interface](#web-interface)
- [API Endpoints](#api-endpoints)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)

## ✨ Features

- **Real-time Monitoring**: Track temperature, humidity, weight, and air quality
- **Web Dashboard**: Mobile-responsive interface with live updates
- **SMS Alerts**: Automatic notifications when thresholds are exceeded
- **Data Logging**: SQLite database for historical data storage
- **Monthly Reports**: Generate and export CSV reports
- **Secure Authentication**: Session-based login system
- **LCD Display**: Local 16x2 display for quick readings
- **Customizable Alerts**: Configure alert thresholds via web interface

## 🔧 Hardware Requirements

| Component | Specification | Quantity |
|-----------|--------------|----------|
| ESP32 Development Board | ESP32-WROOM-32 | 1 |
| DHT22 Sensor | Temperature & Humidity | 1 |
| HX711 Load Cell Amplifier | 24-bit ADC | 1 |
| Load Cell | 50kg or appropriate for hive | 1 |
| MQ-135 Gas Sensor | Air Quality Monitor | 1 |
| 16x2 I2C LCD Display | 0x3F address | 1 |
| SIM800L GSM Module | For SMS functionality | 1 |
| Power Supply | 5V, 2A minimum | 1 |
| Jumper Wires | Male-to-Female | Various |

## 📚 Software Requirements

### Arduino IDE Libraries

Install the following libraries through Arduino IDE Library Manager or GitHub:

1. **DHT Sensor Library** by Adafruit
   - [GitHub](https://github.com/adafruit/DHT-sensor-library)
   - Also install: Adafruit Unified Sensor

2. **HX711 Library** by Bogde
   - [GitHub](https://github.com/bogde/HX711)

3. **LiquidCrystal I2C** by Frank de Brabander
   - [GitHub](https://github.com/johnrickman/LiquidCrystal_I2C)

4. **SQLite3 for ESP32** by Siara
   - [GitHub](https://github.com/siara-cc/esp32_arduino_sqlite3_lib)

5. **ESP32 WebServer** (Built-in with ESP32 board support)

### Board Support

Install ESP32 board support in Arduino IDE:
- Add to Board Manager URLs: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- Install "ESP32 by Espressif Systems"

## 🚀 Installation

### Step 1: Clone the Repository

```bash
git clone https://github.com/yourusername/esp32-beehive-monitor.git
cd esp32-beehive-monitor
```

### Step 2: Install Libraries

Open Arduino IDE and install all required libraries listed above.

### Step 3: Configure Settings

Edit the following in `beehive_monitor.ino`:

```cpp
// WiFi Credentials
const char* WIFI_SSID = "Your_WiFi_Name";
const char* WIFI_PASS = "Your_WiFi_Password";

// Login Credentials
const char* login_user = "admin";
const char* login_pass = "admin";

// SMS Default Number
const char* SMS_RECIPIENT = "+1234567890";

// Timezone (GMT offset in seconds)
const long gmtOffset_sec = 10800; // 3 hours for EAT
```

### Step 4: Upload to ESP32

1. Connect ESP32 to computer via USB
2. Select correct board: Tools → Board → ESP32 Arduino → ESP32 Dev Module
3. Select correct port: Tools → Port → (your COM port)
4. Click Upload button

## 🔌 Wiring Diagram

### Pin Connections

| Sensor/Module | ESP32 Pin | Notes |
|---------------|-----------|-------|
| DHT22 Data | GPIO 4 | Pull-up resistor recommended |
| MQ-135 Analog | GPIO 35 | ADC1 channel |
| HX711 DOUT | GPIO 33 | |
| HX711 SCK | GPIO 32 | |
| LCD SDA | GPIO 21 | I2C Data |
| LCD SCL | GPIO 22 | I2C Clock |
| SIM800 TX | GPIO 16 | Connect to ESP32 RX |
| SIM800 RX | GPIO 17 | Connect to ESP32 TX |

### Power Considerations

- ESP32: 5V via USB or VIN pin
- SIM800L: Requires stable 3.7-4.2V, high current (2A peaks)
- Sensors: 3.3V or 5V depending on module

## ⚙️ Configuration

### First Time Setup

1. Power on the ESP32
2. Wait for WiFi connection (LCD will show IP address)
3. Open web browser and navigate to the IP address shown
4. Login with default credentials:
   - Username: `admin`
   - Password: `admin`

### Calibrating Sensors

#### HX711 Load Cell

```cpp
// In setup(), after scale.begin():
scale.set_scale(2280.f);  // Adjust this calibration factor
scale.tare();             // Reset to zero
```

To calibrate:
1. Place known weight on scale
2. Adjust calibration factor until reading is accurate
3. Re-upload code

#### MQ-135 Air Quality

The MQ-135 requires 24-48 hour burn-in period for stable readings. Calibration values depend on your environment.

## 💻 Usage

### Accessing the Dashboard

1. Connect to the same network as ESP32
2. Navigate to ESP32's IP address (shown on LCD)
3. Login with credentials
4. View real-time sensor data

### Setting Alert Thresholds

1. Navigate to "Alert Settings & Limits" section
2. Click Edit icon (pencil)
3. Modify values:
   - Phone Number: SMS recipient
   - Max Temp: Temperature threshold (°C)
   - Max Hum: Humidity threshold (%)
   - Max Kg: Weight threshold
   - Max Air: Air quality threshold
4. Click Save

### Viewing Reports

1. Click "View Monthly Report" button
2. View 30-day daily averages
3. Options:
   - Print Report
   - Download CSV

## 🌐 Web Interface

### Login Page
- Secure session-based authentication
- 30-minute session timeout
- Honeycomb-themed design

### Dashboard
- Live sensor readings (updates every 2 seconds)
- Configurable alert settings
- Recent history table (updates every 30 seconds)
- Test SMS functionality

### Report Page
- 30-day daily averages
- Print-friendly layout
- CSV export functionality

## 🔗 API Endpoints

| Endpoint | Method | Auth Required | Description |
|----------|--------|---------------|-------------|
| `/` | GET | No | Login page |
| `/login` | POST | No | Authenticate user |
| `/logout` | GET | Yes | End session |
| `/dashboard` | GET | Yes | Main dashboard |
| `/data` | GET | Yes | Current sensor readings (JSON) |
| `/get_settings` | GET | Yes | Current alert settings (JSON) |
| `/save_settings` | POST | Yes | Update alert settings |
| `/sms` | POST | Yes | Send test SMS |
| `/history` | GET | Yes | Last 10 readings (JSON) |
| `/monthly_data` | GET | Yes | 30-day averages (JSON) |
| `/report` | GET | Yes | Monthly report page |

### Example API Response

```json
{
  "temp": 28.50,
  "hum": 45.60,
  "weight": 19.00,
  "air": 995
}
```

## 🐛 Troubleshooting

### WiFi Connection Issues

- Verify SSID and password in code
- Check WiFi signal strength
- ESP32 supports 2.4GHz only (not 5GHz)
- Monitor Serial output at 115200 baud

### Sensor Reading Errors

- **DHT22 returns NaN**: Check wiring, add 10kΩ pull-up resistor
- **HX711 not ready**: Verify power supply, check connections
- **MQ-135 unstable**: Allow 24-48hr burn-in period

### SMS Not Sending

- Verify SIM card has credit and signal
- Check SIM800L power supply (needs 2A capability)
- Monitor Serial output for AT command responses
- Verify phone number format (+countrycode...)

### Database Issues

- If corruption occurs, reflash ESP32 (erases LittleFS)
- Check available storage: Serial output shows diagnostics
- WAL mode requires adequate free heap memory

### Web Interface Not Loading

- Clear browser cache
- Try different browser
- Check if ESP32 IP changed (DHCP)
- Verify firewall settings

## 🤝 Contributing

Contributions are welcome! Please follow these guidelines:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Development Guidelines

- Comment your code
- Follow existing code style
- Test thoroughly before submitting
- Update documentation as needed

## 📝 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- **Adafruit** - DHT sensor library
- **Bogde** - HX711 library
- **Siara** - ESP32 SQLite implementation
- **SVG Repo** - Icons used in web interface
- **ESP32 Community** - Documentation and support

## 📧 Contact

Project Link: [https://github.com/yourusername/esp32-beehive-monitor](https://github.com/yourusername/esp32-beehive-monitor)

---

Made with ❤️ for beekeepers everywhere 🐝