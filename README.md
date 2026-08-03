# 💧 ESP32 Rainwater Tank Monitor

ESP32 firmware for monitoring rainwater tank levels using a 4–20 mA pressure sensor (TL136).

## ✨ Features
- **Accurate Level Measurement**: Reads 4–20 mA current loop via ADC.
- **Web UI & REST API**: Responsive interface with dynamic tank animation and detailed metrics.
- **Home Assistant Ready**: Automatic MQTT discovery for volume and error sensors.
- **Visual Status**: RGB LED feedback for status and low-water/full-tank alerts.
- **Over-The-Air (OTA)**: Remote firmware updates.

## 🛠️ Hardware Setup
- **MCU**: ESP32
- **Sensor**: TL136 Submersible Liquid Level Transmitter (4–20 mA)
- **Sensor Pin**: GPIO 34 (with 150 Ω sampling resistor)
- **Status LED**: GPIO 26 (R), GPIO 27 (G), GPIO 14 (B)

## 🚀 Getting Started
1. Open the project in Arduino IDE or PlatformIO.
2. Update the Wi-Fi credentials and MQTT broker IP in the code.
3. Flash the code to your ESP32.
