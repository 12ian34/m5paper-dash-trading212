"""
Fetches Trading 212 data + moon phase, writes dashboard.json.
Run via cron every few minutes.
"""

import base64
import json
import math
import os
from datetime import datetime, timezone

import httpx
from dotenv import load_dotenv

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(SCRIPT_DIR, "..", ".env"))

TRADING212_API_KEY = os.environ["TRADING212_API_KEY"]
TRADING212_API_SECRET = os.environ["TRADING212_API_SECRET"]
TRADING212_BASE = "https://live.trading212.com/api/v0"

DASHBOARD_PATH = os.path.join(SCRIPT_DIR, "dashboard.json")
DAILY_BASELINE_PATH = os.path.join(SCRIPT_DIR, "daily_baseline.json")


def t212_auth_header() -> dict[str, str]:
    creds = base64.b64encode(
        f"{TRADING212_API_KEY}:{TRADING212_API_SECRET}".encode()
    ).decode()
    return {"Authorization": f"Basic {creds}"}


def load_daily_baseline() -> dict:
    if os.path.exists(DAILY_BASELINE_PATH):
        with open(DAILY_BASELINE_PATH) as f:
            return json.load(f)
    return {}


def save_daily_baseline(data: dict) -> None:
    with open(DAILY_BASELINE_PATH, "w") as f:
        json.dump(data, f)


def get_daily_pnl(current_value: float) -> tuple[float, float]:
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    baseline = load_daily_baseline()

    if baseline.get("date") != today:
        save_daily_baseline({"date": today, "value": current_value})
        return 0.0, 0.0

    baseline_value = baseline["value"]
    daily_pnl = current_value - baseline_value
    daily_pct = (daily_pnl / baseline_value * 100) if baseline_value else 0
    return daily_pnl, daily_pct


def moon_phase() -> dict:
    """
    Calculate current moon phase using a known new moon reference.
    Returns phase name, age in days, and illumination percentage.
    """
    # Reference new moon: 2000-01-06 18:14 UTC
    ref = datetime(2000, 1, 6, 18, 14, 0, tzinfo=timezone.utc)
    now = datetime.now(timezone.utc)
    days_since = (now - ref).total_seconds() / 86400
    cycle = 29.53058867
    age = days_since % cycle
    fraction = age / cycle

    # Illumination: 0 at new moon, 1 at full, back to 0
    illumination = (1 - math.cos(2 * math.pi * fraction)) / 2

    # Phase name
    if fraction < 0.0625:
        name = "New Moon"
    elif fraction < 0.1875:
        name = "Waxing Crescent"
    elif fraction < 0.3125:
        name = "First Quarter"
    elif fraction < 0.4375:
        name = "Waxing Gibbous"
    elif fraction < 0.5625:
        name = "Full Moon"
    elif fraction < 0.6875:
        name = "Waning Gibbous"
    elif fraction < 0.8125:
        name = "Last Quarter"
    elif fraction < 0.9375:
        name = "Waning Crescent"
    else:
        name = "New Moon"

    return {
        "name": name,
        "age_days": round(age, 1),
        "illumination_pct": round(illumination * 100, 1),
    }


def sun_times() -> dict:
    """
    Calculate sunrise/sunset for London using simplified solar equations.
    Returns times as HH:MM strings in local UK time (UTC or BST).
    """
    now = datetime.now(timezone.utc)
    # London coordinates
    lat = 51.5074
    lng = -0.1278

    # Day of year
    n = now.timetuple().tm_yday

    # Solar declination (radians)
    decl = math.radians(-23.44 * math.cos(math.radians(360 / 365 * (n + 10))))

    # Hour angle for sunrise/sunset
    lat_rad = math.radians(lat)
    cos_ha = -math.tan(lat_rad) * math.tan(decl)
    cos_ha = max(-1, min(1, cos_ha))  # clamp for polar edge cases
    ha = math.degrees(math.acos(cos_ha))

    # Solar noon in hours UTC (approximate, based on longitude)
    solar_noon = 12.0 - lng / 15.0

    sunrise_h = solar_noon - ha / 15.0
    sunset_h = solar_noon + ha / 15.0

    # UK timezone offset (simple BST check: last Sunday of March to last Sunday of October)
    month = now.month
    if 4 <= month <= 9:
        offset = 1  # BST
    elif month == 3 and now.day >= 25:
        offset = 1
    elif month == 10 and now.day < 25:
        offset = 1
    else:
        offset = 0  # GMT

    sunrise_h += offset
    sunset_h += offset

    def fmt(h: float) -> str:
        h = h % 24
        return f"{int(h):02d}:{int((h % 1) * 60):02d}"

    return {
        "sunrise": fmt(sunrise_h),
        "sunset": fmt(sunset_h),
    }


def fetch_trading212() -> dict:
    headers = t212_auth_header()
    with httpx.Client(timeout=15.0) as client:
        resp = client.get(f"{TRADING212_BASE}/equity/account/summary", headers=headers)
        resp.raise_for_status()
        account = resp.json()

    total_value = account.get("totalValue", 0)
    investments = account.get("investments", {})
    total_cost = investments.get("totalCost", 0)
    unrealised_pnl = investments.get("unrealizedProfitLoss", 0)
    pnl_pct = (unrealised_pnl / total_cost * 100) if total_cost else 0
    daily_pnl, daily_pct = get_daily_pnl(total_value)

    return {
        "pnl_pct": round(pnl_pct, 2),
        "daily_pnl": round(daily_pnl, 2),
        "daily_pct": round(daily_pct, 2),
    }


def main():
    widgets = {}

    try:
        widgets["trading212"] = fetch_trading212()
    except Exception as e:
        widgets["trading212"] = {"error": str(e)}

    widgets["moon"] = moon_phase()
    widgets["sun"] = sun_times()

    dashboard = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "widgets": widgets,
    }

    with open(DASHBOARD_PATH, "w") as f:
        json.dump(dashboard, f, indent=2)

    print(f"[{dashboard['timestamp']}] Updated dashboard.json")


if __name__ == "__main__":
    main()
