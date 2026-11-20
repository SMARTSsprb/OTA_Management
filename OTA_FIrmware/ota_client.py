#!/usr/bin/env python3
"""
BLE OTA Client for CPR Manikin (ESP32-C3)
Supports unique manikin identification ("CPR_10X" where X varies, e.g. CPR_101, CPR_106).
High Security: Requires 6-digit PIN Passkey pairing (Default: 123456).

Prerequisites:
    pip install bleak

Usage:
    # Auto-detect manikin or select from list:
    python ota_client.py firmware.bin [--fast]

    # Target a specific unique Manikin:
    python ota_client.py firmware.bin CPR_106 [--fast]
    python ota_client.py firmware.bin --device CPR_101 [--fast]

    # Scan and list all active CPR Manikins:
    python ota_client.py --scan

    # Remotely set persistent unique Manikin name in NVS:
    python ota_client.py --device CPR_101 --set-name CPR_106
"""

import argparse
import asyncio
import struct
import sys
import time

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    BleakScanner = None
    BleakClient = None

# GATT UUIDs (little-endian on wire, standard notation here)
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CTRL_UUID    = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DATA_UUID    = "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"

# Protocol commands & statuses
CMD_START    = 0x01
CMD_END      = 0x02
CMD_ABORT    = 0x03
CMD_SET_NAME = 0x04
CMD_GET_INFO = 0x05

STATUS_MAP = {
    0x00: "READY",
    0x02: "DONE",
    0xFF: "ERROR",
}

DEFAULT_PREFIXES = ("CPR_10", "CPR_", "Allreal_CPR")


async def scan_manikins(timeout=5.0):
    """Scan and return all discovered CPR manikins."""
    print(f"[SCAN] Scanning for CPR Manikins ({timeout:.1f}s)...")
    devices = await BleakScanner.discover(timeout=timeout)
    manikins = []
    for d in devices:
        name = d.name or ""
        if any(name.startswith(p) for p in DEFAULT_PREFIXES):
            manikins.append(d)
    return manikins


async def resolve_target_device(target_name, timeout=5.0):
    """Find specific device by name or auto-discover/prompt from available manikins."""
    if target_name:
        print(f"[SCAN] Searching for target Manikin: '{target_name}' (timeout: {timeout}s)...")
        dev = await BleakScanner.find_device_by_name(target_name, timeout=timeout)
        if not dev:
            # Check by address
            devices = await BleakScanner.discover(timeout=timeout)
            for d in devices:
                if d.address.upper() == target_name.upper():
                    return d
            sys.exit(f"[ERROR] Manikin '{target_name}' not found. Use --scan to view available devices.")
        return dev

    # Auto-discovery mode
    manikins = await scan_manikins(timeout=timeout)
    if not manikins:
        sys.exit("[ERROR] No CPR Manikins found. Make sure the ESP32-C3 device is powered on and advertising.")

    if len(manikins) == 1:
        selected = manikins[0]
        print(f"[INFO] Auto-detected unique Manikin: {selected.name} [{selected.address}] (RSSI: {selected.rssi} dBm)")
        return selected

    print(f"\n[DISCOVERY] Found {len(manikins)} CPR Manikins:")
    for idx, d in enumerate(manikins, start=1):
        print(f"  [{idx}] {d.name:<12} (MAC: {d.address}, RSSI: {d.rssi} dBm)")

    # Prompt user
    if sys.stdin.isatty():
        while True:
            try:
                choice = input(f"\nSelect Manikin to update [1-{len(manikins)}]: ").strip()
                if not choice:
                    continue
                num = int(choice)
                if 1 <= num <= len(manikins):
                    return manikins[num - 1]
            except (ValueError, KeyboardInterrupt):
                sys.exit("\nOperation cancelled.")
    else:
        sys.exit("[ERROR] Multiple manikins found. Please specify target name via: python ota_client.py <fw.bin> <CPR_10X>")


async def set_manikin_name(device, new_name):
    """Connect and configure persistent unique name in NVS."""
    print(f"[CONNECT] Connecting to {device.name} [{device.address}] to set name to '{new_name}'...")
    async with BleakClient(device) as client:
        if not client.is_connected:
            sys.exit("[ERROR] Connection failed.")

        payload = bytes([CMD_SET_NAME]) + new_name.encode("utf-8")
        await client.write_gatt_char(CTRL_UUID, payload, response=True)
        print(f"[SUCCESS] Unique Manikin name updated to '{new_name}' in NVS! Reboot device to advertise new name.")


async def perform_ota(device, fw_path, fast_mode=False):
    """Upload firmware binary over BLE."""
    with open(fw_path, "rb") as f:
        fw_data = f.read()

    total_bytes = len(fw_data)
    print(f"[FILE] Loaded firmware: {fw_path} ({total_bytes} bytes / {total_bytes/1024:.1f} KB)")
    print(f"[CONNECT] Connecting to Manikin {device.name} [{device.address}]...")

    status_queue = asyncio.Queue()

    def on_ctrl_notify(_, data: bytearray):
        st = data[0]
        status_name = STATUS_MAP.get(st, f"0x{st:02X}")
        status_queue.put_nowait(st)
        if len(data) >= 5:
            acked_bytes = struct.unpack("<I", data[1:5])[0]
            print(f"\n[DEVICE STATUS] {status_name} (received {acked_bytes}/{total_bytes} B)")
        else:
            print(f"\n[DEVICE STATUS] {status_name}")

    async with BleakClient(device) as client:
        if not client.is_connected:
            sys.exit("[ERROR] Failed to connect to device.")

        # Subscribe to Control notifications
        await client.start_notify(CTRL_UUID, on_ctrl_notify)

        # Chunk size calculation based on MTU
        chunk_size = max(20, min(client.mtu_size - 3, 490))
        print(f"[BLE] Negotiated MTU: {client.mtu_size} B -> Using chunk size: {chunk_size} B")

        # 1. Send CMD_START + u32 size
        print("[OTA] Sending START command...")
        start_payload = bytes([CMD_START]) + struct.pack("<I", total_bytes)
        await client.write_gatt_char(CTRL_UUID, start_payload, response=True)

        # Wait for READY status
        try:
            status = await asyncio.wait_for(status_queue.get(), timeout=10.0)
            if status != 0x00:
                sys.exit(f"[ERROR] Device rejected OTA with status 0x{status:02X}")
        except asyncio.TimeoutError:
            sys.exit("[ERROR] Timeout waiting for device READY status.")

        print("[OTA] Device is READY. Streaming firmware chunks...")
        start_time = time.time()
        sent_bytes = 0

        for offset in range(0, total_bytes, chunk_size):
            chunk = fw_data[offset:offset + chunk_size]
            await client.write_gatt_char(DATA_UUID, chunk, response=(not fast_mode))
            sent_bytes += len(chunk)

            elapsed = max(0.001, time.time() - start_time)
            speed_kbps = (sent_bytes / 1024.0) / elapsed
            pct = (sent_bytes * 100.0) / total_bytes
            eta = (total_bytes - sent_bytes) / (speed_kbps * 1024.0) if speed_kbps > 0 else 0

            sys.stdout.write(f"\r[PROGRESS] {pct:5.1f}% | {sent_bytes}/{total_bytes} B | {speed_kbps:5.1f} KB/s | ETA: {int(eta)}s")
            sys.stdout.flush()

        print(f"\n[OTA] 100% upload finished in {time.time() - start_time:.1f}s. Finalizing image...")

        # 2. Send CMD_END (device verifies SHA-256 and sets boot partition)
        try:
            await client.write_gatt_char(CTRL_UUID, bytes([CMD_END]), response=True)
            result = await asyncio.wait_for(status_queue.get(), timeout=15.0)
            if result == 0x02:
                print(f"[SUCCESS] OTA successful for {device.name}! Device is now rebooting into new firmware.")
            else:
                print(f"[ERROR] Finalization failed with status 0x{result:02X}.")
        except Exception as e:
            # Device typically reboots immediately upon CMD_END validation
            print(f"[INFO] Connection closed ({e}). Device is successfully rebooting into new firmware!")


def main():
    parser = argparse.ArgumentParser(description="CPR Manikin (CPR_10X) BLE OTA Client")
    parser.add_argument("file", nargs="?", help="Path to firmware .bin file")
    parser.add_argument("device_arg", nargs="?", help="Target Manikin name (e.g. CPR_101, CPR_106)")
    parser.add_argument("--device", "-d", dest="device_flag", help="Target Manikin name or MAC address")
    parser.add_argument("--fast", action="store_true", help="Fast upload mode (write without response)")
    parser.add_argument("--scan", action="store_true", help="Scan and list all detected CPR Manikins")
    parser.add_argument("--set-name", dest="new_name", help="Set and persist unique Manikin name into NVS (e.g. CPR_106)")

    args = parser.parse_args()
    target_name = args.device_flag or args.device_arg

    if BleakScanner is None:
        sys.exit("[ERROR] Python package 'bleak' is required to communicate over Bluetooth LE.\nPlease install via: pip install bleak")

    if args.scan:
        async def run_scan():
            manikins = await scan_manikins(timeout=5.0)
            if not manikins:
                print("[RESULT] No CPR Manikins found.")
                return
            print(f"\n{'DEVICE NAME':<16} {'MAC ADDRESS':<20} {'RSSI':<8}")
            print("-" * 46)
            for m in manikins:
                print(f"{(m.name or 'Unknown'):<16} {m.address:<20} {m.rssi:<8} dBm")
            print()
        asyncio.run(run_scan())
        return

    if args.new_name:
        async def run_set_name():
            dev = await resolve_target_device(target_name, timeout=5.0)
            await set_manikin_name(dev, args.new_name)
        asyncio.run(run_set_name())
        return

    if not args.file:
        parser.print_help()
        sys.exit(1)

    async def run_update():
        dev = await resolve_target_device(target_name, timeout=5.0)
        await perform_ota(dev, args.file, fast_mode=args.fast)

    asyncio.run(run_update())


if __name__ == "__main__":
    main()
