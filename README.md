# CPR Manikin BLE OTA Management Subsystem

Secure Bluetooth Low Energy (BLE) Over-The-Air (OTA) firmware update system for ESP32-C3 CPR training manikins.

## Features

- **Unique Manikin Identification (`CPR_10X`):**
  - Manikin name format: `CPR_10X` (e.g., `CPR_101`, `CPR_102`, `CPR_106`).
  - Persistent NVS storage for customized manikin names.
  - Automatic hardware MAC fallback ensures zero name collisions across multiple units.
- **High Security & Encrypted Communication:**
  - Enforces LE Secure Connections (ECDH encryption with AES-128).
  - MITM (Man-In-The-Middle) protection enabled.
  - Enforced 6-digit Passkey authentication (`BLE_GATT_CHR_F_WRITE_AUTHEN` and `BLE_GATT_CHR_F_WRITE_ENC`) on GATT Control and Data characteristics.
  - Default secure pairing PIN: `123456` (configurable at compile-time or in NVS).
- **Dual Client Tools:**
  - **Python CLI Tool (`ota_client.py`):** Scan, target specific manikin, upload firmware with live speed/ETA, and remotely configure unique names.
  - **Web Bluetooth Updater (`UI/Allreal_CPR_OTA_Updater.html`):** In-browser zero-install OTA updater supporting Chrome, Edge, and Bluefy with native passkey pairing.

## Directory Structure

```text
OTA_Management/
├── OTA_FIrmware/
│   ├── main/
│   │   └── CMakeLists.txt        # Component registration
│   ├── CMakeLists.txt           # ESP-IDF project definition
│   ├── main.c                   # NimBLE GATT OTA driver + Security Manager
│   ├── ota_client.py            # Python Bleak OTA client
│   ├── partitions.csv           # 4MB dual-slot OTA partition table
│   └── sdkconfig.defaults       # ESP32-C3 NimBLE & Security settings
├── UI/
│   └── Allreal_CPR_OTA_Updater.html # Web Bluetooth browser updater
└── README.md
```

## Quick Start

### 1. Flash OTA Firmware to ESP32-C3

```bash
cd OTA_FIrmware
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

### 2. Update Firmware via Python Client

```bash
pip install bleak

# Discover all active CPR Manikins
python ota_client.py --scan

# Target specific manikin
python ota_client.py app_firmware.bin CPR_106 --fast
```

When prompted by the OS, enter the 6-digit passkey: `123456`.

### 3. Update Firmware via Web Browser

1. Open `UI/Allreal_CPR_OTA_Updater.html` in Chrome or Edge.
2. Click **Connect to CPR Manikin**.
3. Select your manikin from the list (e.g., `CPR_106`).
4. Enter the passkey `123456` in the Bluetooth pairing popup.
5. Drop your `.bin` firmware file and click **Upload & Update Firmware**.
