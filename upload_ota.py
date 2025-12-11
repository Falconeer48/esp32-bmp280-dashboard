#!/usr/bin/env python3
"""
Simple OTA upload script for ESP32 using ArduinoOTA protocol
"""
import socket
import sys
import os

def upload_ota(ip, port, firmware_path):
    """Upload firmware via OTA"""
    try:
        # Read the firmware binary
        with open(firmware_path, 'rb') as f:
            firmware = f.read()
        
        # Connect to ESP32
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        print(f"Connecting to {ip}:{port}...")
        sock.connect((ip, port))
        
        # Send firmware size first (4 bytes, little-endian)
        size = len(firmware)
        sock.sendall(size.to_bytes(4, 'little'))
        
        # Send firmware data in chunks
        chunk_size = 1024
        sent = 0
        while sent < size:
            chunk = firmware[sent:sent + chunk_size]
            sock.sendall(chunk)
            sent += len(chunk)
            print(f"Uploaded {sent}/{size} bytes ({sent*100//size}%)", end='\r')
        
        print(f"\nUpload complete! ({size} bytes)")
        sock.close()
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        return False

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python3 upload_ota.py <IP> <PORT> <FIRMWARE_BIN>")
        sys.exit(1)
    
    ip = sys.argv[1]
    port = int(sys.argv[2])
    firmware_path = sys.argv[3]
    
    if not os.path.exists(firmware_path):
        print(f"Error: Firmware file not found: {firmware_path}")
        sys.exit(1)
    
    upload_ota(ip, port, firmware_path)

