"""
Fetches Trading 212 data (account summary + positions), writes dashboard.json.
Run via cron at :29 and :59 past every hour.
"""

import base64
import json
import os
from datetime import datetime, timedelta, timezone

import httpx
from dotenv import load_dotenv

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(SCRIPT_DIR, "..", ".env"))

TRADING212_API_KEY = os.environ["TRADING212_API_KEY"]
TRADING212_API_SECRET = os.environ["TRADING212_API_SECRET"]
TRADING212_BASE = "https://live.trading212.com/api/v0"

DASHBOARD_PATH = os.path.join(SCRIPT_DIR, "dashboard.json")
ROLLING_BASELINE_PATH = os.path.join(SCRIPT_DIR, "rolling_24h_baseline.json")
ROLLING_WINDOW_HOURS = 24
HISTORY_RETENTION_HOURS = 72


def t212_auth_header() -> dict[str, str]:
    creds = base64.b64encode(
        f"{TRADING212_API_KEY}:{TRADING212_API_SECRET}".encode()
    ).decode()
    return {"Authorization": f"Basic {creds}"}


def parse_snapshot_time(raw_ts: str) -> datetime | None:
    ts = raw_ts.strip()
    if ts.endswith("Z"):
        ts = f"{ts[:-1]}+00:00"
    try:
        parsed = datetime.fromisoformat(ts)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def load_rolling_history() -> list[dict]:
    if not os.path.exists(ROLLING_BASELINE_PATH):
        return []

    try:
        with open(ROLLING_BASELINE_PATH) as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError):
        return []

    raw_snapshots = []
    if isinstance(data, dict) and isinstance(data.get("snapshots"), list):
        raw_snapshots = data["snapshots"]
    elif isinstance(data, list):
        raw_snapshots = data
    elif isinstance(data, dict) and "value" in data and "positions" in data:
        # Legacy daily-baseline format: treat it as a single seed snapshot.
        legacy_date = data.get("date")
        if isinstance(legacy_date, str):
            seed_ts = f"{legacy_date}T00:00:00+00:00"
        else:
            seed_ts = datetime.now(timezone.utc).isoformat()
        raw_snapshots = [
            {
                "timestamp": seed_ts,
                "value": data.get("value", 0),
                "positions": data.get("positions", {}),
            }
        ]

    clean_snapshots = []
    for snapshot in raw_snapshots:
        if not isinstance(snapshot, dict):
            continue
        raw_ts = snapshot.get("timestamp") or snapshot.get("ts")
        if not isinstance(raw_ts, str):
            continue
        parsed_ts = parse_snapshot_time(raw_ts)
        if parsed_ts is None:
            continue

        raw_value = snapshot.get("value")
        if not isinstance(raw_value, (int, float)):
            continue

        raw_positions = snapshot.get("positions")
        clean_positions: dict[str, float] = {}
        if isinstance(raw_positions, dict):
            for ticker, price in raw_positions.items():
                if isinstance(ticker, str) and isinstance(price, (int, float)):
                    clean_positions[ticker] = float(price)

        clean_snapshots.append(
            {
                "timestamp": parsed_ts.isoformat(),
                "value": float(raw_value),
                "positions": clean_positions,
            }
        )

    clean_snapshots.sort(key=lambda s: s["timestamp"])
    return clean_snapshots


def save_rolling_history(snapshots: list[dict]) -> None:
    with open(ROLLING_BASELINE_PATH, "w") as f:
        json.dump({"snapshots": snapshots}, f)


def build_position_price_map(positions: list[dict]) -> dict[str, float]:
    price_map: dict[str, float] = {}
    for position in positions:
        if not isinstance(position, dict):
            continue
        instrument = position.get("instrument", {})
        ticker = instrument.get("ticker")
        price = position.get("currentPrice")
        if isinstance(ticker, str) and isinstance(price, (int, float)):
            price_map[ticker] = float(price)
    return price_map


def append_snapshot(
    history: list[dict], now_utc: datetime, total_value: float, positions: list[dict]
) -> list[dict]:
    history.append(
        {
            "timestamp": now_utc.isoformat(),
            "value": float(total_value),
            "positions": build_position_price_map(positions),
        }
    )

    cutoff = now_utc - timedelta(hours=HISTORY_RETENTION_HOURS)
    trimmed = []
    for snapshot in history:
        ts = snapshot.get("timestamp")
        if not isinstance(ts, str):
            continue
        parsed_ts = parse_snapshot_time(ts)
        if parsed_ts is None:
            continue
        if parsed_ts >= cutoff:
            trimmed.append(snapshot)

    trimmed.sort(key=lambda s: s["timestamp"])
    return trimmed


def pick_rolling_baseline(history: list[dict], now_utc: datetime) -> dict | None:
    if not history:
        return None

    target = now_utc - timedelta(hours=ROLLING_WINDOW_HOURS)
    candidate = None
    for snapshot in history:
        ts = snapshot.get("timestamp")
        if not isinstance(ts, str):
            continue
        parsed_ts = parse_snapshot_time(ts)
        if parsed_ts is None:
            continue
        if parsed_ts <= target:
            candidate = snapshot
        else:
            break

    # Warm-up mode (<24h history): fall back to earliest available snapshot.
    return candidate if candidate is not None else history[0]


def to_plain_symbol(ticker: str) -> str:
    """Convert T212 ticker to plain symbol (e.g. AAPL_US_EQ -> AAPL)."""
    base = ticker.replace("_EQ", "")
    if "_US" in base:
        return base.replace("_US", "").replace("_", ".")
    if base and base[-1] in "ladp":
        return base[:-1]
    return base


def to_display_name(instrument: dict) -> str:
    """Return a readable company name from a Trading212 instrument."""
    name = instrument.get("name")
    if isinstance(name, str) and name.strip():
        clean_name = name.strip()
        if len(clean_name) > 24:
            return f"{clean_name[:21]}..."
        return clean_name

    ticker = instrument.get("ticker", "")
    return to_plain_symbol(ticker) if isinstance(ticker, str) else "???"


def fetch_trading212() -> dict:
    headers = t212_auth_header()
    with httpx.Client(timeout=15.0) as client:
        resp = client.get(
            f"{TRADING212_BASE}/equity/account/summary", headers=headers
        )
        resp.raise_for_status()
        account = resp.json()

        resp = client.get(f"{TRADING212_BASE}/equity/positions", headers=headers)
        resp.raise_for_status()
        positions = resp.json()

    # Overall P&L
    investments = account.get("investments", {})
    total_cost = investments.get("totalCost", 0)
    unrealised_pnl = investments.get("unrealizedProfitLoss", 0)
    pnl_pct = (unrealised_pnl / total_cost * 100) if total_cost else 0

    # Rolling 24h tracking (account-level + per-position baselines)
    total_value = account.get("totalValue", 0)
    now_utc = datetime.now(timezone.utc)
    history = load_rolling_history()
    history = append_snapshot(history, now_utc, total_value, positions)
    save_rolling_history(history)
    baseline = pick_rolling_baseline(history, now_utc)

    if baseline is None:
        daily_pnl = 0.0
        daily_pct = 0.0
        pos_baselines = {}
    else:
        baseline_value = baseline.get("value", 0)
        daily_pnl = total_value - baseline_value
        daily_pct = (daily_pnl / baseline_value * 100) if baseline_value else 0
        pos_baselines = baseline.get("positions", {})

    # Per-position 24h change + overall change
    daily_movers = []
    overall_movers = []
    for p in positions:
        instrument = p.get("instrument", {})
        ticker = instrument.get("ticker", "")
        if not isinstance(ticker, str):
            continue
        label = to_display_name(instrument)
        current_price = p["currentPrice"]

        # 24h % change (vs rolling baseline)
        baseline_price = pos_baselines.get(ticker)
        if baseline_price and baseline_price > 0:
            daily_change = (current_price - baseline_price) / baseline_price * 100
        else:
            daily_change = 0.0
        daily_movers.append({"ticker": label, "pct": round(daily_change, 2)})

        # Overall % change (vs purchase price, in account currency)
        wi = p.get("walletImpact", {})
        cost = wi.get("totalCost", 0)
        pnl = wi.get("unrealizedProfitLoss", 0)
        overall_change = (pnl / cost * 100) if cost else 0.0
        overall_movers.append({"ticker": label, "pct": round(overall_change, 2)})

    # 24h window: top 4 winners, bottom 4 losers
    daily_movers.sort(key=lambda x: x["pct"], reverse=True)
    winners = daily_movers[:4]
    losers = sorted(daily_movers[max(0, len(daily_movers) - 4):], key=lambda x: x["pct"])

    # Overall: top 4 best, bottom 4 worst
    overall_movers.sort(key=lambda x: x["pct"], reverse=True)
    best_overall = overall_movers[:4]
    worst_overall = sorted(
        overall_movers[max(0, len(overall_movers) - 4):], key=lambda x: x["pct"]
    )

    return {
        "pnl_pct": round(pnl_pct, 2),
        # Keep field name for firmware compatibility; value is now rolling 24h.
        "daily_pct": round(daily_pct, 2),
        "winners": winners,
        "losers": losers,
        "best_overall": best_overall,
        "worst_overall": worst_overall,
    }


def main():
    widgets = {}

    try:
        widgets["trading212"] = fetch_trading212()
    except Exception as e:
        widgets["trading212"] = {"error": str(e)}

    dashboard = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "widgets": widgets,
    }

    with open(DASHBOARD_PATH, "w") as f:
        json.dump(dashboard, f, indent=2)

    print(f"[{dashboard['timestamp']}] Updated dashboard.json")


if __name__ == "__main__":
    main()
