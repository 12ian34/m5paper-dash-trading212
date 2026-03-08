"""
Fetches Trading 212 data (account summary + positions), writes dashboard.json.
Run via cron at :29 and :59 past every hour.
"""

import base64
import json
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

    # Daily tracking (account-level + per-position baselines)
    total_value = account.get("totalValue", 0)
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    baseline = load_daily_baseline()

    if baseline.get("date") != today:
        # New day — save current values as baselines
        pos_baselines = {}
        for p in positions:
            instrument = p.get("instrument", {})
            ticker = instrument.get("ticker")
            if not isinstance(ticker, str):
                continue
            pos_baselines[ticker] = p.get("currentPrice", 0)
        save_daily_baseline(
            {"date": today, "value": total_value, "positions": pos_baselines}
        )
        daily_pnl = 0.0
        daily_pct = 0.0
    else:
        baseline_value = baseline["value"]
        daily_pnl = total_value - baseline_value
        daily_pct = (daily_pnl / baseline_value * 100) if baseline_value else 0
        pos_baselines = baseline.get("positions", {})

        # Track any new positions bought today
        updated = False
        for p in positions:
            instrument = p.get("instrument", {})
            t = instrument.get("ticker")
            if not isinstance(t, str):
                continue
            if t not in pos_baselines:
                pos_baselines[t] = p["currentPrice"]
                updated = True
        if updated:
            baseline["positions"] = pos_baselines
            save_daily_baseline(baseline)

    # Per-position daily change + overall change
    daily_movers = []
    overall_movers = []
    for p in positions:
        instrument = p.get("instrument", {})
        ticker = instrument.get("ticker", "")
        if not isinstance(ticker, str):
            continue
        label = to_display_name(instrument)
        current_price = p["currentPrice"]

        # Daily % change (vs start-of-day baseline)
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

    # Daily: top 4 winners, bottom 4 losers
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
