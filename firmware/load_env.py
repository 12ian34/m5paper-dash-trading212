"""
PlatformIO pre-build script: reads ../.env and injects values
as -D compiler flags so the firmware can use them as constants.
"""

import os

Import("env")

env_path = os.path.join(env.get("PROJECT_DIR"), "..", ".env")

if not os.path.exists(env_path):
    print(f"WARNING: {env_path} not found, skipping env injection")
else:
    with open(env_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()
            # Only inject firmware-relevant vars
            if key in ("WIFI_SSID", "WIFI_PASS", "DASHBOARD_URL"):
                # Escape quotes for C string
                escaped = value.replace('"', '\\"')
                env.Append(CPPDEFINES=[(key, f'\\"{escaped}\\"')])
                print(f"  ENV -> {key}={'*' * min(len(value), 8)}")
