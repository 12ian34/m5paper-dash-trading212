#!/bin/bash
# Run on the Pi to set up cron jobs for the M5 dashboard.
# Usage: bash server/setup_cron.sh (from repo root)
# Set M5_DASH_SERVER_DIR if you want to override the default path.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${M5_DASH_SERVER_DIR:-$SCRIPT_DIR}"
VENV="$DIR/.venv/bin/python"
UPDATE_SCRIPT="$DIR/update_dashboard.py"
SERVE_SCRIPT="$DIR/serve.py"

if [ ! -f "$UPDATE_SCRIPT" ] || [ ! -f "$SERVE_SCRIPT" ]; then
    echo "Could not find dashboard scripts in: $DIR"
    echo "Set M5_DASH_SERVER_DIR to the directory containing update_dashboard.py and serve.py."
    exit 1
fi

# Ensure venv exists
if [ ! -f "$VENV" ]; then
    echo "Setting up venv..."
    cd "$DIR"
    uv venv
    uv pip install -r requirements.txt
fi

# Build cron entries
UPDATE_JOB="29,59 * * * * cd $DIR && $VENV $UPDATE_SCRIPT >> $DIR/cron.log 2>&1"
SERVER_JOB="@reboot cd $DIR && $VENV $SERVE_SCRIPT 8080 > $DIR/http.log 2>&1 &"

# Add to crontab (preserving existing entries)
{ crontab -l 2>/dev/null | grep -v -E 'update_dashboard\.py|serve\.py 8080|python3 -m http\.server 8080|http\.server 8080'; printf '%s\n%s\n' "$UPDATE_JOB" "$SERVER_JOB"; } | crontab -

echo "Cron jobs installed:"
crontab -l | grep -E 'update_dashboard|serve.py 8080'
echo ""
echo "Starting file server now..."
pkill -f "serve.py 8080" 2>/dev/null || true
pkill -f "http.server 8080" 2>/dev/null || true
cd "$DIR" && "$VENV" "$SERVE_SCRIPT" 8080 > "$DIR/http.log" 2>&1 &
echo "Running initial update..."
cd "$DIR" && "$VENV" update_dashboard.py
echo "Done! Dashboard at http://$(hostname -I | awk '{print $1}'):8080/dashboard.json"
