#!/system/bin/sh
# customize.sh — Magisk/KernelSU/APatch module installer script
# This runs when flashing via manager app or recovery

SKIP_MOUNT=false
PROPFILE=false
POSTFSDATA=false
LATESTARTSERVICE=true

print_modname() {
    ui_print "─────────────────────────────────────────"
    ui_print "   FaceGate Virtual Camera"
    ui_print "   Version: $MODVER"
    ui_print "─────────────────────────────────────────"
}

on_install() {
    ui_print "- Installing to $MODPATH"

    # Extract module contents from ZIP
    unzip -o "$ZIPFILE" 'system/*'  -d "$MODPATH" >&2
    unzip -o "$ZIPFILE" 'zygisk/*'  -d "$MODPATH" >&2
    unzip -o "$ZIPFILE" 'service.sh' -d "$MODPATH" >&2
    unzip -o "$ZIPFILE" 'post-fs-data.sh' -d "$MODPATH" >&2
    unzip -o "$ZIPFILE" 'sepolicy.rule' -d "$MODPATH" >&2

    # itsanon_injector (ptrace-based, AndKittyInjector) — one binary per ABI,
    # lives under system/bin so it's already extracted by the 'system/*'
    # unzip above. We just verify it made it in and chmod it explicitly
    # since executables need 0755 regardless of the recursive perm pass below.
    for BIN in "$MODPATH/system/bin/itsanon_injector64" "$MODPATH/system/bin/itsanon_injector32"; do
        if [ -f "$BIN" ]; then
            chmod 0755 "$BIN"
            ui_print "- Injector binary: $(basename "$BIN") ✓"
        fi
    done

    # ── Manager-specific handling ──
    if [ -n "$KSU" ] && [ "$KSU" = "true" ]; then
        # KernelSU
        ui_print "- KernelSU detected"
        # KernelSU uses OverlayFS - need metamodule for system/ mount
        if [ ! -d "/data/adb/modules/meta-overlayfs" ] && [ ! -d "/data/adb/modules/overlayfs" ]; then
            ui_print ""
            ui_print "! WARNING: No metamodule detected for OverlayFS."
            ui_print "! Install meta-overlayfs for system/ modifications to work."
            ui_print ""
        fi
    elif [ -d "/data/adb/ap" ]; then
        # APatch
        ui_print "- APatch detected"
    else
        # Magisk
        ui_print "- Magisk detected"
        # Verify Zygisk is enabled
        if [ "$(magisk --sqlite "SELECT value FROM settings WHERE key='zygisk';" 2>/dev/null)" != "value=1" ]; then
            ui_print ""
            ui_print "! WARNING: Zygisk appears to be DISABLED."
            ui_print "! Enable Zygisk and re-flash this module."
            ui_print ""
        fi
    fi

    # Check Android version
    local api_level=$(getprop ro.build.version.sdk)
    if [ "$api_level" -lt 28 ]; then
        ui_print "! ERROR: Android API $api_level < 28 — requires Android 9+"
        abort "Unsupported Android version"
    fi
    ui_print "- Android API $api_level ✓"

    # ── Set permissions ──
    # Magisk: set_perm_recursive defaults to system_file context
    # KernelSU: set_perm_recursive defaults to system_file context
    # We override with appropriate context for each manager

    if [ -n "$KSU" ] && [ "$KSU" = "true" ]; then
        # KernelSU: system/ needs system_file for OverlayFS
        set_perm_recursive "$MODPATH/system" root root 0755 0644 u:object_r:system_file:s0
        set_perm_recursive "$MODPATH/zygisk" root root 0755 0644 u:object_r:system_file:s0
    elif [ -d "/data/adb/ap" ]; then
        # APatch: same as KernelSU
        set_perm_recursive "$MODPATH/system" root root 0755 0644 u:object_r:system_file:s0
        set_perm_recursive "$MODPATH/zygisk" root root 0755 0644 u:object_r:system_file:s0
    else
        # Magisk: can use magisk_file (auto) or system_file
        set_perm_recursive "$MODPATH/system" root root 0755 0644 u:object_r:system_file:s0
        set_perm_recursive "$MODPATH/zygisk" root root 0755 0644 u:object_r:magisk_file:s0
    fi

    # Scripts always executable
    set_perm "$MODPATH/service.sh" root root 0755
    set_perm "$MODPATH/post-fs-data.sh" root root 0755
    set_perm "$MODPATH/sepolicy.rule" root root 0644

    # Injector binaries (ptrace-based, run as root shell processes)
    for BIN in "$MODPATH/system/bin/itsanon_injector64" "$MODPATH/system/bin/itsanon_injector32"; do
        [ -f "$BIN" ] && set_perm "$BIN" root root 0755
    done

    # Initialize safety files
    mkdir -p "$MODPATH/facegate"
    echo 0 > "$MODPATH/facegate/crash_count"
    echo "active" > "$MODPATH/facegate/safety_state"

    ui_print "- Installation complete."
    ui_print "- Reboot once to let the module load. After that, the camera"
    ui_print "  hook injects into cameraserver automatically (ptrace) — no"
    ui_print "  further reboots or manual steps needed, even if cameraserver"
    ui_print "  restarts later."
}
