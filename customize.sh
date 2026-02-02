#!/system/bin/sh
# MockGPS Installation Script

MODDIR=${0%/*}

ui_print "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ui_print "  MockGPS v2.0.0"
ui_print "  GPS Spoofing + Dev Options Hide"
ui_print "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Create default config if not exists
CONFIG="/data/adb/modules/mockgps/location.conf"
if [ ! -f "$CONFIG" ]; then
    mkdir -p /data/adb/modules/mockgps
    cat > "$CONFIG" <<'EOF'
enabled=0
lat=0.0
lng=0.0
accuracy=5.0
altitude=0.0
speed=0.0
bearing=0.0
hidedev=1
EOF
    chmod 0644 "$CONFIG"
    ui_print "  Created default config"
fi

# Set permissions
set_perm_recursive $MODDIR 0 0 0755 0644
for f in $MODDIR/zygisk/*.so; do
    [ -f "$f" ] && chmod 0644 "$f"
done

ui_print ""
ui_print "  Install companion app for map UI"
ui_print "  Config: $CONFIG"
ui_print ""
ui_print "  Reboot to activate"
ui_print "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
