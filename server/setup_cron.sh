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

# Ensure .venv exists and dependencies are up to date
echo "Syncing Python environment with uv..."
cd "$DIR"
uv sync

# Build cron entries
CRON_TAG="m5paper-dash-trading212"
UPDATE_JOB="29,59 * * * * cd $DIR && $VENV $UPDATE_SCRIPT >> $DIR/cron.log 2>&1 # ${CRON_TAG}:update"
SERVER_JOB="@reboot cd $DIR && $VENV $SERVE_SCRIPT 8080 > $DIR/http.log 2>&1 & # ${CRON_TAG}:serve"

# Add to crontab (preserving existing entries)
CURRENT_CRON="$(crontab -l 2>/dev/null || true)"
CLEANED_CRON="$(printf '%s\n' "$CURRENT_CRON" \
  | grep -v -F "${CRON_TAG}:" \
  | grep -v -F "cd $DIR && $VENV update_dashboard.py" \
  | grep -v -F "cd $DIR && $VENV $UPDATE_SCRIPT" \
  | grep -v -F "cd $DIR && python3 $DIR/serve.py 8080" \
  | grep -v -F "cd $DIR && $VENV $SERVE_SCRIPT 8080" \
  | grep -v -E 'python3 -m http\.server 8080|http\.server 8080')"
{ printf '%s\n' "$CLEANED_CRON"; printf '%s\n%s\n' "$UPDATE_JOB" "$SERVER_JOB"; } | crontab -

echo "Cron jobs installed:"
crontab -l | grep -F "${CRON_TAG}:"
echo ""
echo "Starting file server now..."
pkill -f "serve.py 8080" 2>/dev/null || true
pkill -f "http.server 8080" 2>/dev/null || true
cd "$DIR" && "$VENV" "$SERVE_SCRIPT" 8080 > "$DIR/http.log" 2>&1 &
echo "Running initial update..."
cd "$DIR" && "$VENV" update_dashboard.py
echo "Done! Dashboard at http://$(hostname -I | awk '{print $1}'):8080/dashboard.json"
