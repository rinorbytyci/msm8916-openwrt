#!/bin/sh
# /usr/sbin/update-display
# Multi-mode display update script
# Mode selected via /tmp/display_mode (0=default, 1=stats, 2=network, 3=wireguard)

MODE_FILE="/tmp/display_mode"
MODE=$(cat "$MODE_FILE" 2>/dev/null || echo "0")

# === BATTERY (shared by all modes) ===
BMS_PATH="/sys/class/power_supply/pm8916-bms-vm"
VOLTAGE=$(cat "$BMS_PATH/voltage_now" 2>/dev/null)
VMAX=$(cat "$BMS_PATH/voltage_max_design" 2>/dev/null)
VMIN=$(cat "$BMS_PATH/voltage_min_design" 2>/dev/null)
CHARGING_STATUS=$(cat "$BMS_PATH/status" 2>/dev/null)
CHARGING_FLAG=""
[ "$CHARGING_STATUS" = "Charging" ] && CHARGING_FLAG="-c"

if [ -n "$VOLTAGE" ] && [ -n "$VMAX" ] && [ -n "$VMIN" ]; then
    BATTERY=$(awk "BEGIN {pct = (($VOLTAGE - $VMIN) / ($VMAX - $VMIN)) * 100; if(pct < 0) pct=0; if(pct > 100) pct=100; printf \"%.0f\", pct}")
else
    BATTERY=100
fi

# Helper: convert bytes to human-readable
human_bytes() {
    awk -v b="$1" 'BEGIN {split("B K M G T",u); s=1; while(b>=1024 && s<5){b/=1024; s++} printf "%.1f%s", b, u[s]}'
}

case "$MODE" in
    1)
        # === SYSTEM STATS MODE ===
        UPTIME=$(awk '{d=int($1/86400); h=int(($1%86400)/3600); m=int(($1%3600)/60); if(d>0) printf "%dd%dh", d, h; else if(h>0) printf "%dh%dm", h, m; else printf "%dm", m}' /proc/uptime)
        RAM=$(free -m | awk '/^Mem:/ {printf "%d/%dM", $3, $2}')
        LOAD=$(awk '{print $1}' /proc/loadavg)
        TEMP_RAW=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
        if [ -n "$TEMP_RAW" ]; then
            TEMP="$((TEMP_RAW/1000))C"
        else
            TEMP="N/A"
        fi

        /usr/bin/router-display -m 1 $CHARGING_FLAG -b "$BATTERY" \
            -U "$UPTIME" -R "$RAM" -L "$LOAD" -T "$TEMP" > /dev/fb0
        ;;
    2)
        # === NETWORK MODE ===
        # Find any modem/WAN interface that actually has an IPv4 address
        WAN_IF=""
        WAN_IP=""
        for iface in wwan0 usb0 rmnet_data0 rmnet0 rmnet_ipa0 eth0; do
            IP=$(ip -4 addr show dev "$iface" 2>/dev/null | awk '/inet / {print $2}' | cut -d/ -f1 | head -1)
            if [ -n "$IP" ]; then
                WAN_IF="$iface"
                WAN_IP="$IP"
                break
            fi
        done
        [ -z "$WAN_IP" ] && WAN_IP="None"

        SIGNAL=""
        SIG=""
        QUAL=""
        MODEM_IDX=$(mmcli -L 2>/dev/null | grep -oE 'Modem/[0-9]+' | head -1 | cut -d'/' -f2)
        if [ -n "$MODEM_IDX" ]; then
            SIG=$(mmcli -m "$MODEM_IDX" --signal-get 2>/dev/null | grep -oE 'rssi: [-0-9.]+' | head -1 | awk '{print $2}')
            if [ -n "$SIG" ]; then
                # Strip decimals for compactness: -67.0 -> -67
                SIG_INT=$(printf '%.0f' "$SIG" 2>/dev/null)
                SIGNAL="${SIG_INT}dB"
            else
                QUAL=$(mmcli -m "$MODEM_IDX" -K 2>/dev/null | grep 'signal-quality.value' | awk -F': ' '{print $2}' | tr -d "'")
                [ -n "$QUAL" ] && SIGNAL="${QUAL}%"
            fi
        fi

        # Derive 0-4 signal bars from RSSI (dBm) or signal quality (%)
        SIGNAL_BARS=$(awk -v s="$SIG" -v q="$QUAL" 'BEGIN {
            if (s != "" && s+0 != 0) {
                if (s+0 >= -65) print 4
                else if (s+0 >= -75) print 3
                else if (s+0 >= -85) print 2
                else if (s+0 >= -95) print 1
                else print 0
            } else if (q != "") {
                if (q+0 >= 75) print 4
                else if (q+0 >= 50) print 3
                else if (q+0 >= 25) print 2
                else if (q+0 >= 10) print 1
                else print 0
            } else print 0
        }')

        RX_B=0
        TX_B=0
        if [ -n "$WAN_IF" ]; then
            RX_B=$(cat /sys/class/net/$WAN_IF/statistics/rx_bytes 2>/dev/null || echo 0)
            TX_B=$(cat /sys/class/net/$WAN_IF/statistics/tx_bytes 2>/dev/null || echo 0)
        fi
        RX=$(human_bytes "$RX_B")
        TX=$(human_bytes "$TX_B")

        CLIENTS=$(iw dev phy0-ap0 station dump 2>/dev/null | grep -c "^Station")
        [ -z "$CLIENTS" ] && CLIENTS=0

        /usr/bin/router-display -m 2 $CHARGING_FLAG -b "$BATTERY" \
            -I "$WAN_IP" -G "$SIGNAL" -g "$SIGNAL_BARS" \
            -X "$RX" -Y "$TX" -C "$CLIENTS" > /dev/fb0
        ;;
    3)
        # === WIREGUARD MODE ===
        WG_IF=$(wg show interfaces 2>/dev/null | awk '{print $1}' | head -1)
        WG_STATUS="Down"
        WG_HS="Never"
        WG_RX="0B"
        WG_TX="0B"

        if [ -n "$WG_IF" ]; then
            HS_TS=$(wg show "$WG_IF" latest-handshakes 2>/dev/null | awk '{print $2}' | head -1)
            if [ -n "$HS_TS" ] && [ "$HS_TS" != "0" ]; then
                NOW=$(date +%s)
                AGO=$((NOW - HS_TS))
                if [ "$AGO" -lt 180 ]; then
                    WG_STATUS="UP"
                else
                    WG_STATUS="Stale"
                fi
                if [ "$AGO" -lt 60 ]; then
                    WG_HS="${AGO}s"
                elif [ "$AGO" -lt 3600 ]; then
                    WG_HS="$((AGO/60))m"
                else
                    WG_HS="$((AGO/3600))h"
                fi
            fi
            TRANSFER=$(wg show "$WG_IF" transfer 2>/dev/null | head -1)
            if [ -n "$TRANSFER" ]; then
                RX_B=$(echo "$TRANSFER" | awk '{print $2}')
                TX_B=$(echo "$TRANSFER" | awk '{print $3}')
                WG_RX=$(human_bytes "$RX_B")
                WG_TX=$(human_bytes "$TX_B")
            fi
        fi

        # System uptime (shown as "Up:" on the VPN screen)
        SYS_UPTIME=$(awk '{d=int($1/86400); h=int(($1%86400)/3600); m=int(($1%3600)/60); if(d>0) printf "%dd%dh", d, h; else if(h>0) printf "%dh%dm", h, m; else printf "%dm", m}' /proc/uptime)

        /usr/bin/router-display -m 3 $CHARGING_FLAG -b "$BATTERY" \
            -W "$WG_STATUS" -H "$WG_HS" -a "$SYS_UPTIME" \
            -D "$WG_RX" -E "$WG_TX" > /dev/fb0
        ;;
    4)
        # === WIFI MODE ===
        SSID=$(uci get wireless.@wifi-iface[0].ssid 2>/dev/null || echo "WiFi")
        PASSWORD=$(uci get wireless.@wifi-iface[0].key 2>/dev/null || echo "(open)")
        CLIENTS=$(iw dev phy0-ap0 station dump 2>/dev/null | grep -c "^Station")
        [ -z "$CLIENTS" ] && CLIENTS=0

        /usr/bin/router-display -m 4 $CHARGING_FLAG -b "$BATTERY" \
            -s "$SSID" -p "$PASSWORD" -C "$CLIENTS" > /dev/fb0
        ;;
    5)
        # === NOTES MODE ===
        # Priority: UCI system.notes -> /etc/display_notes file -> system.description
        NOTES=$(uci get system.@system[0].notes 2>/dev/null)
        [ -z "$NOTES" ] && NOTES=$(cat /etc/display_notes 2>/dev/null)
        [ -z "$NOTES" ] && NOTES=$(uci get system.@system[0].description 2>/dev/null)

        /usr/bin/router-display -m 5 $CHARGING_FLAG -b "$BATTERY" \
            -N "$NOTES" > /dev/fb0
        ;;
    *)
        # === DEFAULT MODE (QR / operator / signal bars / battery) ===
        QR_FLAG=""
        if ip link show phy0-ap0 2>/dev/null | grep -q "state UP"; then
            QR_FLAG="-q"
        fi

        OPERATOR_CACHE="/tmp/modem_operator.cache"
        MODEM_IDX=$(mmcli -L 2>/dev/null | grep -oE 'Modem/[0-9]+' | head -1 | cut -d'/' -f2)

        OPERATOR=""
        NETWORK=""
        SIG=""
        QUAL=""
        if [ -n "$MODEM_IDX" ]; then
            MODEM_DATA=$(mmcli -m "$MODEM_IDX" -K 2>/dev/null)

            if [ -f "$OPERATOR_CACHE" ]; then
                OPERATOR=$(cat "$OPERATOR_CACHE")
            else
                SIM_IDX=$(echo "$MODEM_DATA" | grep 'modem.generic.sim' | awk -F': ' '{print $2}' | grep -oE '[0-9]+$')
                if [ -n "$SIM_IDX" ]; then
                    OPERATOR=$(mmcli -i "$SIM_IDX" -K 2>/dev/null | grep 'sim.properties.operator-name' | awk -F': ' '{print $2}')
                fi
                if [ -z "$OPERATOR" ]; then
                    OPERATOR=$(echo "$MODEM_DATA" | grep 'modem.3gpp.operator-name' | awk -F': ' '{print $2}')
                fi
                if [ -n "$OPERATOR" ]; then
                    echo "$OPERATOR" > "$OPERATOR_CACHE"
                fi
            fi

            ACCESS_TECH=$(echo "$MODEM_DATA" | grep 'modem.generic.access-technologies' | awk -F': ' '{print $2}')
            if [ -n "$ACCESS_TECH" ] && [ "$ACCESS_TECH" != "unknown" ]; then
                case "$ACCESS_TECH" in
                    *lte*) NETWORK="4G" ;;
                    *umts*|*hspa*|*hsupa*|*hsdpa*) NETWORK="3G" ;;
                    *edge*|*gprs*|*gsm*) NETWORK="2G" ;;
                    *) NETWORK="$ACCESS_TECH" ;;
                esac
            fi

            # Signal for bars
            SIG=$(mmcli -m "$MODEM_IDX" --signal-get 2>/dev/null | grep -oE 'rssi: [-0-9.]+' | head -1 | awk '{print $2}')
            if [ -z "$SIG" ]; then
                QUAL=$(echo "$MODEM_DATA" | grep 'signal-quality.value' | awk -F': ' '{print $2}' | tr -d "'")
            fi
        fi

        SIGNAL_BARS=$(awk -v s="$SIG" -v q="$QUAL" 'BEGIN {
            if (s != "" && s+0 != 0) {
                if (s+0 >= -65) print 4
                else if (s+0 >= -75) print 3
                else if (s+0 >= -85) print 2
                else if (s+0 >= -95) print 1
                else print 0
            } else if (q != "") {
                if (q+0 >= 75) print 4
                else if (q+0 >= 50) print 3
                else if (q+0 >= 25) print 2
                else if (q+0 >= 10) print 1
                else print 0
            } else print 0
        }')

        SSID=$(uci get wireless.@wifi-iface[0].ssid 2>/dev/null || echo "WiFi")
        PASSWORD=$(uci get wireless.@wifi-iface[0].key 2>/dev/null || echo "")
        HOSTNAME=$(uci get system.@system[0].hostname 2>/dev/null || echo "Router")

        /usr/bin/router-display -m 0 $QR_FLAG $CHARGING_FLAG \
            -b "$BATTERY" -n "$OPERATOR" -t "$NETWORK" -g "$SIGNAL_BARS" \
            -s "$SSID" -p "$PASSWORD" -h "$HOSTNAME" > /dev/fb0
        ;;
esac
