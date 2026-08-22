#!/system/bin/sh

MODDIR="${0%/*}"

# Ensure log directory exists
mkdir -p "$MODDIR/facegate"

# ── Detect which root manager is active ──
if [ -n "$KSU" ] && [ "$KSU" = "true" ]; then
    # KernelSU detected
    MANAGER="KernelSU"
elif [ -d "/data/adb/ap" ]; then
    # APatch detected
    MANAGER="APatch"
else
    # Default to Magisk
    MANAGER="Magisk"
fi

# Log to file
echo "[post-fs-data] Manager: $MANAGER" >> "$MODDIR/facegate/service.log"

# ── Set SELinux contexts based on manager ──
case "$MANAGER" in
    "Magisk")
        # Magisk auto-labels module files as magisk_file via magiskinit
        # We just ensure the context is correct
        if [ -d "$MODDIR/system" ]; then
            chcon -R u:object_r:magisk_file:s0 "$MODDIR/system" 2>/dev/null || true
        fi
        # Also label zygisk libs
        if [ -d "$MODDIR/zygisk" ]; then
            chcon -R u:object_r:magisk_file:s0 "$MODDIR/zygisk" 2>/dev/null || true
        fi
        echo "[post-fs-data] Set magisk_file context" >> "$MODDIR/facegate/service.log"
        ;;

    "KernelSU"|"APatch")
        # KernelSU/APatch use OverlayFS for system/ overlay
        # Files in system/ need system_file context to be readable by system processes
        if [ -d "$MODDIR/system" ]; then
            chcon -R u:object_r:system_file:s0 "$MODDIR/system" 2>/dev/null || true
            echo "[post-fs-data] Set system_file context on system/" >> "$MODDIR/facegate/service.log"
        fi
        # Zygisk libs and other files can use magisk_file or system_file
        if [ -d "$MODDIR/zygisk" ]; then
            chcon -R u:object_r:system_file:s0 "$MODDIR/zygisk" 2>/dev/null || true
        fi
        # Module root and scripts
        chcon u:object_r:system_file:s0 "$MODDIR" 2>/dev/null || true
        chcon u:object_r:system_file:s0 "$MODDIR/post-fs-data.sh" 2>/dev/null || true
        chcon u:object_r:system_file:s0 "$MODDIR/service.sh" 2>/dev/null || true
        ;;
esac

# ── Verify hook libraries exist ──
if [ -f "$MODDIR/system/lib64/libhookProxy.so" ]; then
    echo "[post-fs-data] Hook lib (64-bit): OK" >> "$MODDIR/facegate/service.log"
else
    echo "[post-fs-data] WARNING: system/lib64/libhookProxy.so missing" >> "$MODDIR/facegate/service.log"
fi

if [ -f "$MODDIR/system/lib/libhookProxy.so" ]; then
    echo "[post-fs-data] Hook lib (32-bit): OK" >> "$MODDIR/facegate/service.log"
else
    echo "[post-fs-data] WARNING: system/lib/libhookProxy.so missing" >> "$MODDIR/facegate/service.log"
fi

# ── Initialize safety state files ──
if [ ! -f "$MODDIR/disable" ]; then
    [ -f "$MODDIR/facegate/crash_count" ] || echo 0 > "$MODDIR/facegate/crash_count"
    [ -f "$MODDIR/facegate/safety_state" ] || echo "active" > "$MODDIR/facegate/safety_state"
fi

echo "[post-fs-data] Done" >> "$MODDIR/facegate/service.log"
