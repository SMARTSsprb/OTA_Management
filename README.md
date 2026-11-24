# CPR Manikin BLE OTA Management Subsystem

Secure Bluetooth Low Energy (BLE) Over-The-Air (OTA) firmware update system for ESP32-C3 CPR training manikins.

## Features

- **Unique Manikin Identification (`CPR_10X`):**
  - Manikin name format: `CPR_10X` (e.g., `CPR_101`, `CPR_102`, `CPR_103`, `CPR_106`).
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
- **Automated Cloud CI/CD:**
  - Pre-configured GitHub Actions workflow automatically builds the OTA firmware and packages a single `allreal_cpr_ota_merged_0x0.bin` ready to flash.

## Directory Structure

```text
OTA_Management/
├── .github/
│   └── workflows/
│       └── build-ota.yml        # Automated ESP-IDF GitHub Actions builder
├── OTA_FIrmware/
│   ├── main/
│   │   ├── CMakeLists.txt       # Component registration
│   │   └── main.c               # NimBLE GATT OTA driver + Security Manager
│   ├── CMakeLists.txt           # ESP-IDF project definition
│   ├── ota_client.py            # Python Bleak OTA client
│   ├── partitions.csv           # 4MB dual-slot OTA partition table
│   └── sdkconfig.defaults       # ESP32-C3 NimBLE & Security settings
├── UI/
│   └── Allreal_CPR_OTA_Updater.html # Web Bluetooth browser updater
└── README.md
```

## Quick Start

### 1. Build and Flash OTA Firmware (First Time Setup)

#### Option A: Download from GitHub Actions (Recommended)
1. Go to your GitHub repository: `https://github.com/SMARTSsprb/OTA_Management/actions`
2. Select the latest **Build ESP32-C3 OTA Firmware** workflow run.
3. Download the `CPR_OTA_Firmware_ESP32C3` artifact.
4. Extract the zip file and flash the merged binary directly to the board:
   ```bash
   python -m esptool --chip esp32c3 -p COM14 write-flash 0x0 allreal_cpr_ota_merged_0x0.bin
   ```

#### Option B: Build Locally with ESP-IDF
```bash
cd OTA_FIrmware
idf.py set-target esp32c3
idf.py build
idf.py -p COM14 flash monitor
```

---

### 2. Update Firmware via Web Browser (Zero-Install)

1. Open `UI/Allreal_CPR_OTA_Updater.html` in Google Chrome or Microsoft Edge.
2. Click **Connect to CPR Manikin**.
3. Select your manikin from the list (e.g., `CPR_103`).
4. When prompted by Windows/Chrome for the Bluetooth passkey, enter: **`123456`**.
5. Drag and drop your target application `.bin` file (e.g. `zephyr_103.bin`).
6. Click **Upload & Update Firmware**.

---

### 3. Update Firmware via Python CLI

```bash
pip install bleak

# Discover all active CPR Manikins
python ota_client.py --scan

# Target specific manikin with Passkey
python ota_client.py zephyr_103.bin CPR_103 --passkey 123456 --fast
```
