#!/bin/bash
# Run on the Pi to set up cron jobs for the M5 dashboard.
# Usage: bash ~/dev/m5/server/setup_cron.sh

DIR="$HOME/dev/m5/server"
VENV="$DIR/.venv/bin/python"

# Ensure venv exists
if [ ! -f "$VENV" ]; then
    echo "Setting up venv..."
    cd "$DIR"
    uv venv
    uv pip install -r requirements.txt
fi

# Build cron entries
UPDATE_JOB="*/5 * * * * cd $DIR && $VENV update_dashboard.py >> $DIR/cron.log 2>&1"
SERVER_JOB="@reboot cd $DIR && python3 -m http.server 8080 > $DIR/http.log 2>&1 &"

# Add to crontab (preserving existing entries)
(crontab -l 2>/dev/null | grep -v 'update_dashboard\|http.server 8080'; echo "$UPDATE_JOB"; echo "$SERVER_JOB") | crontab -

echo "Cron jobs installed:"
crontab -l | grep -E 'update_dashboard|http.server'
echo ""
echo "Starting file server now..."
pkill -f "http.server 8080" 2>/dev/null || true
cd "$DIR" && python3 -m http.server 8080 > "$DIR/http.log" 2>&1 &
echo "Running initial update..."
cd "$DIR" && "$VENV" update_dashboard.py
echo "Done! Dashboard at http://$(hostname -I | awk '{print $1}'):8080/dashboard.json"
