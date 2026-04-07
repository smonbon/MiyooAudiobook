#!/bin/sh
cd $(dirname "$0")

# CPU performance mode
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# Stop audio server so we can use the sound device
. /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh

# Use our bundled libraries (copied from device) + device fallbacks
export LD_LIBRARY_PATH=$(pwd)/lib:/mnt/SDCARD/.tmp_update/lib/parasyte:/mnt/SDCARD/miyoo/lib:/customer/lib:$LD_LIBRARY_PATH
export LD_PRELOAD=./lib/libSDL-1.2.so.0
export HOME=/mnt/SDCARD

chmod +x ./audiobook-player
./audiobook-player 2>log.txt

unset LD_PRELOAD

# Return to OnionOS
/mnt/SDCARD/.tmp_update/runtime.sh &
