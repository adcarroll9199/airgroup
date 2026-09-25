#!/usr/bin/env bash
# Installs airgroup on Raspberry Pi OS (Bookworm or Trixie).
#   sudo ./install.sh "Whole House"                 # AirPlay name = group name
#   sudo ./install.sh "Whole House" "Pi Speakers"   # custom AirPlay name
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Please run with sudo: sudo $0 \"<speaker group name>\"" >&2
  exit 1
fi

SRC="$(cd "$(dirname "$0")" && pwd)"
CAST_TARGET="${1:-}"
AIRPLAY_NAME="${2:-}"
HTTP_PORT="${HTTP_PORT:-8090}"

if [[ -z $CAST_TARGET && -f /etc/default/airgroup ]]; then
  CAST_TARGET="$(sed -n 's/^CAST_TARGET="\(.*\)"$/\1/p' /etc/default/airgroup)"
fi
if [[ -z $CAST_TARGET ]]; then
  read -rp "Google Home speaker group name (exactly as in the Home app): " CAST_TARGET
fi
AIRPLAY_NAME="${AIRPLAY_NAME:-$CAST_TARGET}"

render() {  # render <template> <dest> <mode>
  local text
  text="$(<"$1")"
  text="${text//@CAST_TARGET@/$CAST_TARGET}"
  text="${text//@AIRPLAY_NAME@/$AIRPLAY_NAME}"
  text="${text//@HTTP_PORT@/$HTTP_PORT}"
  printf '%s\n' "$text" > "$2"
  chmod "$3" "$2"
}

echo "==> Installing packages"
apt-get update
apt-get install -y --no-install-recommends shairport-sync ffmpeg python3-venv curl avahi-daemon

echo "==> Installing airgroup"
id airgroup &>/dev/null || useradd --system --no-create-home --shell /usr/sbin/nologin airgroup
install -d /opt/airgroup
install -m 0755 "$SRC/airgroup.py" /opt/airgroup/airgroup.py
[[ -d /opt/airgroup/venv ]] || python3 -m venv /opt/airgroup/venv
/opt/airgroup/venv/bin/pip install --quiet -r "$SRC/requirements.txt"
render "$SRC/airgroup-volume.in" /usr/local/bin/airgroup-volume 0755

if [[ -f /etc/default/airgroup ]]; then
  cp /etc/default/airgroup /etc/default/airgroup.bak
  esc="${CAST_TARGET//\\/\\\\}"; esc="${esc//|/\\|}"; esc="${esc//&/\\&}"
  sed -i "s|^CAST_TARGET=.*|CAST_TARGET=\"$esc\"|" /etc/default/airgroup
else
  render "$SRC/airgroup.env.in" /etc/default/airgroup 0644
fi

echo "==> Configuring shairport-sync"
[[ -f /etc/shairport-sync.conf && ! -f /etc/shairport-sync.conf.orig ]] &&
  cp /etc/shairport-sync.conf /etc/shairport-sync.conf.orig
render "$SRC/shairport-sync.conf.in" /etc/shairport-sync.conf 0644
install -d /etc/systemd/system/shairport-sync.service.d
install -m 0644 "$SRC/shairport-sync-override.conf" /etc/systemd/system/shairport-sync.service.d/airgroup.conf
install -m 0644 "$SRC/airgroup.service" /etc/systemd/system/airgroup.service

# Wi-Fi power saving on the Pi Zero causes audio dropouts and flaky speaker discovery.
if [[ -d /etc/NetworkManager/conf.d ]]; then
  printf '[connection]\nwifi.powersave = 2\n' > /etc/NetworkManager/conf.d/airgroup-wifi-powersave.conf
  systemctl reload NetworkManager || true
fi
iw dev wlan0 set power_save off 2>/dev/null || true

echo "==> Starting services"
systemctl daemon-reload
systemctl enable airgroup shairport-sync
systemctl restart airgroup
systemctl restart shairport-sync

cat <<EOF

Done. On your iPhone, open Apple Music, tap the AirPlay icon and pick "$AIRPLAY_NAME".
Status page:  http://$(hostname -I | awk '{print $1}'):$HTTP_PORT/
Logs:         journalctl -u airgroup -u shairport-sync -f
Find groups:  /opt/airgroup/venv/bin/python /opt/airgroup/airgroup.py --list
EOF
