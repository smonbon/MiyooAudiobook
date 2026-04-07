#!/bin/bash
# ============================================================
#  deploy_wifi.sh – Deploy app wirelessly via SSH/SCP
#
#  One-time setup:
#    1. Connect Miyoo to WiFi: Settings → Network
#    2. Find its IP (shown in Network settings or your router)
#    3. Run: ./scripts/deploy_wifi.sh 192.168.x.x
#       (or set MIYOO_IP below to skip typing it each time)
# ============================================================

set -e

MIYOO_IP="${1:-$MIYOO_IP}"   # argument takes priority, then env var
MIYOO_USER="root"
MIYOO_PORT="22"
REMOTE_DIR="/mnt/SDCARD/App/MiyooAudiobook"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5 -o PasswordAuthentication=yes"

if [ -z "$MIYOO_IP" ]; then
    echo "Usage: ./scripts/deploy_wifi.sh <miyoo-ip>"
    echo "  e.g. ./scripts/deploy_wifi.sh 192.168.1.42"
    echo ""
    echo "Find the IP on your Miyoo: Settings → Network"
    exit 1
fi

echo "Deploying to $MIYOO_USER@$MIYOO_IP:$REMOTE_DIR ..."

# Test connection
if ! ssh $SSH_OPTS -p $MIYOO_PORT $MIYOO_USER@$MIYOO_IP "echo ok" > /dev/null 2>&1; then
    echo ""
    echo "Cannot connect to $MIYOO_IP"
    echo "  • Is the Miyoo powered on and on the same WiFi?"
    echo "  • Check the IP in Settings → Network on the device"
    echo "  • Try pinging: ping $MIYOO_IP"
    exit 1
fi

# Generate icon
python3 "$(dirname "$0")/make_icon.py"

# Create remote directories
ssh $SSH_OPTS -p $MIYOO_PORT $MIYOO_USER@$MIYOO_IP \
    "mkdir -p $REMOTE_DIR/assets $REMOTE_DIR/lib /mnt/SDCARD/Audiobooks"

# Copy files
SCP="scp $SSH_OPTS -P $MIYOO_PORT"
$SCP audiobook-player       $MIYOO_USER@$MIYOO_IP:$REMOTE_DIR/
$SCP scripts/launch.sh      $MIYOO_USER@$MIYOO_IP:$REMOTE_DIR/
$SCP config.json            $MIYOO_USER@$MIYOO_IP:$REMOTE_DIR/
$SCP assets/*               $MIYOO_USER@$MIYOO_IP:$REMOTE_DIR/assets/ 2>/dev/null || true
$SCP lib/*                  $MIYOO_USER@$MIYOO_IP:$REMOTE_DIR/lib/    2>/dev/null || true

# Make binary executable
ssh $SSH_OPTS -p $MIYOO_PORT $MIYOO_USER@$MIYOO_IP "chmod +x $REMOTE_DIR/audiobook-player"

echo ""
echo "Deployed! Files on device:"
ssh $SSH_OPTS -p $MIYOO_PORT $MIYOO_USER@$MIYOO_IP "ls -lh $REMOTE_DIR"
echo ""
echo "Start the app from the Miyoo menu."
