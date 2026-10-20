#!/bin/sh

LINKG="/app/current/bin/linkg"
LOG="/tmp/linkg_watchdog.log"

while true
do
    if ! pidof linkg >/dev/null 2>&1; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') linkg not running, restart" >> "$LOG"

        cd /app/current/bin || exit 1

        "$LINKG" >> /tmp/linkg.log 2>&1 &
    fi

    sleep 1
done
