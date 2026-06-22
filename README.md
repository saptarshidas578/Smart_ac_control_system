# Smart AC Control System

An intelligent AC compressor protection and monitoring system built using the ESP8266. This project monitors compressor temperature, fan temperature, fan current, and the indoor unit control signal to protect the outdoor compressor from overheating, fan failure, and abnormal operating conditions.

---

## Overview

This project intercepts the control signal coming from the indoor unit of an air conditioner and uses multiple sensors to determine whether the outdoor compressor should be allowed to run.

Unlike conventional implementations that use ready-made modules, this project includes custom-designed voltage and current sensing circuits developed from scratch.

---

## Features

* Custom AC voltage sensing circuit
* Homemade current transformer-based current sensor
* Compressor temperature monitoring
* Fan temperature monitoring
* Compressor overheat protection
* Fan thermal protection
* Relay-controlled compressor operation
* WiFi connectivity using ESP8266
* Discord webhook notifications
* NTP time synchronization
* Automatic WiFi reconnection
* Real-time fault monitoring

---

## Working Principle

1. The AC control signal from the indoor unit is detected using a custom voltage sensing circuit.
2. The AC signal is converted to DC and filtered before being fed to the ESP8266 GPIO pin.
3. A homemade current transformer monitors the outdoor fan current.
4. The induced signal is amplified using an operational amplifier and connected to the analog input.
5. PT100 RTD sensors monitor compressor and fan temperatures.
6. The ESP8266 evaluates all sensor readings.
7. If operating conditions are safe, the relay activates the compressor.
8. Fault conditions automatically shut down the compressor and generate alerts.

---

## Hardware Used

* ESP8266 NodeMCU
* MAX31865 RTD Amplifier Modules
* PT100 RTD Sensors
* Relay Module
* Operational Amplifier Circuit
* Homemade Current Transformer
* Custom AC Voltage Sensor Circuit

---

## Safety Features

* Compressor overheat protection
* Fan overheat protection
* Fan failure detection
* Startup stabilization delay
* Shutdown stabilization delay
* Thermal protection cooldown intervals
* Continuous system monitoring

---

## IoT Features

* WiFi connectivity
* Discord webhook notifications
* Time synchronization using NTP
* Remote fault reporting
* System status updates

---

## Software

* PlatformIO
* Arduino Framework
* ESP8266WiFi
* Adafruit MAX31865 Library
* HTTPClient
* NTP Time Synchronization

---

## Project Structure

```text
src/
├── main.cpp

platformio.ini

README.md
```

---

## Future Improvements

* Non-blocking state machine architecture
* EEPROM fault logging
* Web dashboard
* MQTT integration
* Mobile application support
* Data logging and analytics
* OTA firmware updates

---

## Applications

* Air conditioner compressor protection
* HVAC monitoring systems
* Industrial motor protection
* Preventive maintenance systems
* IoT-based equipment monitoring

---

## Author

**Saptarshi Das**

Electronics and Embedded Systems Enthusiast

---

## License

This project is released for educational and research purposes.
