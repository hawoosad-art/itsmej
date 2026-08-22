#!/system/bin/sh
    # service.sh — ptrace injection into cameraserver via amkush_injector
    #
    # Logging: ALL output goes to Android logcat only. No file I/O.
    #   Monitor: adb logcat -s 'amkush/service:V' 'amkush/injector:V'
    #
    # WHY NOT LD_PRELOAD:
    #   AOSP 'neverallow init *:process noatsecure' makes wrap.<service> + LD_PRELOAD
    #   impossible on Android 10+ without an unenforceable sepolicy exception.
    #   Zygisk also cannot help: cameraserver is a native daemon started directly by
    #   init, not forked from zygote.
    #
    # INSTEAD: we ptrace-attach to the already-running cameraserver and
    #   remote-dlopen libhookProxy.so via amkush_injector (AndKittyInjector --pid).
    #   This needs no wrap property, no restart, and no reboot.
    #
    # SELinux: enforcement is disabled for the injection window and immediately
    #   restored. AVC denials and ptrace events are cleared after injection.
    #   WHY HOOKS STAY FUNCTIONAL UNDER ENFORCING after the window:
    #     * libhookProxy.so is already mapped — no new dlopen/mmap needed.
    #     * ShadowHook trampolines live in cameraserver's own anonymous mappings
    #       (cameraserver has execute permission on its own maps).
    #     * processCaptureResult / configureStreams hooks do only memory operations
    #       and gralloc buffer lock/unlock — both are normal cameraserver operations.
    #     * The IPC abstract socket fd is already open; receiving data on an
    #       already-open fd needs no new SELinux transitions.

    MODDIR="${0%/*}"

    # ── State files (machine-readable state, NOT logs) ───────────────────────────
    CRASH_LOG="$MODDIR/facegate/crash_count"
    MAX_CRASHES=3
    COOLDOWN_SEC=30
    WAIT_FOR_CAMERASERVER_SEC=60  # 60 s — cameraserver can be slow on cold boot
    WATCHDOG_INTERVAL_SEC=15
    SELINUX_PATH="/sys/fs/selinux/enforce"

    mkdir -p "$MODDIR/facegate"

    # ── Logcat helpers ────────────────────────────────────────────────────────────
    # /system/bin/log is toybox on all Android 5.0+ (API 21+).
    # Priority flags: v=verbose  d=debug  i=info  w=warn  e=error
    # Monitor all FaceGate events:  adb logcat -s 'amkush/*:V'
    _LOG_TAG="amkush/service"
    log_i() { /system/bin/log -t "$_LOG_TAG" -p i -- "$*" 2>/dev/null || true; }
    log_w() { /system/bin/log -t "$_LOG_TAG" -p w -- "$*" 2>/dev/null || true; }
    log_e() { /system/bin/log -t "$_LOG_TAG" -p e -- "$*" 2>/dev/null || true; }

    log_i "=== service.sh start (ptrace injection mode) ==="
    log_i "MODDIR=$MODDIR"
    log_i "Android: $(getprop ro.build.version.release)  SDK: $(getprop ro.build.version.sdk)"

    # ── Safety: disable flag ──────────────────────────────────────────────────────
    if [ -f "$MODDIR/disable" ]; then
      log_i "Module disabled by flag — exiting"
      exit 0
    fi

    # ── Safety: crash loop detection ─────────────────────────────────────────────
    if [ -f "$CRASH_LOG" ]; then
      CRASH_COUNT=$(cat "$CRASH_LOG" 2>/dev/null || echo 0)
      if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
          LAST_CRASH=$(stat -c %Y "$CRASH_LOG" 2>/dev/null || echo 0)
          NOW=$(date +%s)
          ELAPSED=$((NOW - LAST_CRASH))
          if [ "$ELAPSED" -lt "$COOLDOWN_SEC" ]; then
              log_e "SAFETY: $CRASH_COUNT crashes in ${ELAPSED}s — DISABLING MODULE"
              touch "$MODDIR/disable"
              echo "disabled_crash_loop" > "$MODDIR/facegate/safety_state"
              exit 0
          else
              echo 0 > "$CRASH_LOG"
              log_i "Cooldown passed (${ELAPSED}s) — reset crash counter"
          fi
      fi
    else
      echo 0 > "$CRASH_LOG"
    fi

    # ── Detect root manager ───────────────────────────────────────────────────────
    if [ -n "$KSU" ] && [ "$KSU" = "true" ]; then
      MANAGER="KernelSU"
    elif [ -d "/data/adb/ap" ]; then
      MANAGER="APatch"
    else
      MANAGER="Magisk"
    fi
    log_i "Root manager: $MANAGER"

    # ── Detect device ABI and select injector + hook library ─────────────────────
    ABI=$(getprop ro.product.cpu.abi)
    case "$ABI" in
      arm64-v8a)
          INJECTOR="$MODDIR/system/bin/amkush_injector64"
          [ -f "$INJECTOR" ] || INJECTOR="$MODDIR/system/bin/amkush_injector"
          HOOK="$MODDIR/system/lib64/libhookProxy.so"
          ;;
      armeabi-v7a|armeabi)
          INJECTOR="$MODDIR/system/bin/amkush_injector32"
          [ -f "$INJECTOR" ] || INJECTOR="$MODDIR/system/bin/amkush_injector"
          HOOK="$MODDIR/system/lib/libhookProxy.so"
          ;;
      *)
          log_e "Unsupported ABI '$ABI' — cannot select injector"
          echo "unsupported_abi" > "$MODDIR/facegate/safety_state"
          exit 0
          ;;
    esac
    log_i "ABI=$ABI  injector=$INJECTOR"
    log_i "hook lib=$HOOK"

    if [ ! -f "$INJECTOR" ]; then
      log_e "amkush_injector binary not found: $INJECTOR"
      echo "not_found" > "$MODDIR/facegate/safety_state"
      exit 0
    fi
    if [ ! -f "$HOOK" ]; then
      log_e "libhookProxy.so not found: $HOOK"
      echo "not_found" > "$MODDIR/facegate/safety_state"
      exit 0
    fi
    chmod 0755 "$INJECTOR" 2>/dev/null

    # Check if hookProxy.so is mapped in a process's address space.
    # Works for both filesystem-loaded (.so path) and memfd-loaded (memfd:hookProxy...)
    hook_present() {
      grep -q "hookProxy" "/proc/$1/maps" 2>/dev/null
    }

    # ── Find cameraserver PID ────────────────────────────────────────────
    # cameraserver (/system/bin/cameraserver) is the singleton AOSP Camera Framework
    # daemon -- the ONLY process that loads libcameraservice.so. Started by init at
    # boot and restarted automatically on crash.
    #
    # camerahalserver (/vendor/bin/hw/camerahalserver) is a separate Qualcomm vendor
    # HAL daemon that does NOT load libcameraservice.so. Never target it.
    #
    # pgrep -x gives exact name match. We then verify libcameraservice.so is mapped
    # in that PID as definitive confirmation we have the right process.
    find_camera_pid() {
      local pid
      pid=$(pgrep -x cameraserver 2>/dev/null | head -1)
      if [ -z "$pid" ]; then
        echo ""
        return 1
      fi
      if ! grep -q "libcameraservice.so" "/proc/$pid/maps" 2>/dev/null; then
        log_w "find_camera_pid: PID=$pid matched cameraserver name but libcameraservice.so not mapped"
        echo ""
        return 1
      fi
      echo "$pid"
    }

    # Verify cameraserver PID and libcameraservice.so mapping (AOSP spec).
    verify_cameraserver_pid() {
      local PID
      PID=$(pgrep -x cameraserver 2>/dev/null | head -1)
      if [ -z "$PID" ]; then
        log_e "verify_cameraserver_pid: cameraserver not running"
        return 1
      fi
      if ! grep -q "libcameraservice.so" "/proc/$PID/maps" 2>/dev/null; then
        log_e "verify_cameraserver_pid: libcameraservice.so not mapped in PID=$PID"
        return 1
      fi
      log_i "Verified cameraserver PID=$PID with libcameraservice.so"
      echo "$PID"
    }
    wait_for_cameraserver() {
      local waited=0
      while [ "$waited" -lt "$WAIT_FOR_CAMERASERVER_SEC" ]; do
          local pid
          pid=$(find_camera_pid)
          if [ -n "$pid" ]; then
              log_i "cameraserver found: PID=$pid (waited ${waited}s)"
              echo "$pid"
              return 0
          fi
          sleep 1
          waited=$((waited + 1))
          # Log every 10 s so the terminal widget shows progress
          if [ $((waited % 10)) -eq 0 ]; then
              log_i "Waiting for cameraserver... ${waited}s / ${WAIT_FOR_CAMERASERVER_SEC}s"
          fi
      done
      echo ""
      return 1
    }

    # ── SELinux state helpers ─────────────────────────────────────────────────────
    selinux_get_state() {
      if [ -f "$SELINUX_PATH" ]; then
          cat "$SELINUX_PATH" 2>/dev/null || echo "-1"
      else
          echo "-1"
      fi
    }

    # ── ptrace-inject libhookProxy.so into cameraserver ──────────────────────────
    inject_hook() {
      local pid="$1"
      log_i "=== inject_hook PID=$pid (AndKittyInjector --pid --memfd) ==="

      # Disable SELinux for the injection window
      local prev_selinux
      prev_selinux=$(selinux_get_state)
      log_i "SELinux: was $prev_selinux — setting permissive for injection"
      echo 0 > "$SELINUX_PATH" 2>/dev/null || log_w "SELinux: WARNING: write $SELINUX_PATH failed"
      sleep 0.1

      # Run injector; capture stdout+stderr and relay to logcat line-by-line
      # (subshell for capture preserves the exit code via $?)
      INJECT_OUT=$("$INJECTOR" --pid "$pid" --libs "$HOOK" --memfd --timeout 5000 2>&1)
      local rc=$?

      # Restore SELinux immediately
      if [ "$prev_selinux" = "1" ]; then
          echo 1 > "$SELINUX_PATH" 2>/dev/null || log_w "SELinux: restore enforcing failed"
          log_i "SELinux: restored to enforcing"
      else
          log_i "SELinux: left at $prev_selinux (was not enforcing)"
      fi

      # Emit each injector output line to logcat (tag: amkush/injector)
      if [ -n "$INJECT_OUT" ]; then
          echo "$INJECT_OUT" | while IFS= read -r _line; do
              [ -n "$_line" ] && /system/bin/log -t amkush/injector -p i -- "$_line" 2>/dev/null || true
          done
      fi
      log_i "Injector rc=$rc"

      # Wipe AVC denials and ptrace events written during the permissive window
      dmesg -c > /dev/null 2>&1
      logcat -c > /dev/null 2>&1
      rm -f /sys/fs/pstore/console-ramoops* 2>/dev/null

      if [ "$rc" -eq 0 ] && hook_present "$pid"; then
          log_i "Injection SUCCESS — libhookProxy.so visible in /proc/$pid/maps"
          return 0
      fi
      log_e "Injection FAILED (rc=$rc) — hookProxy NOT in /proc/$pid/maps"
      return 1
    }

    # ── Initial injection ─────────────────────────────────────────────────────────
    CS_PID=$(wait_for_cameraserver)
    if [ -z "$CS_PID" ]; then
      log_e "cameraserver did not appear within ${WAIT_FOR_CAMERASERVER_SEC}s — watchdog will retry"
    else
      if hook_present "$CS_PID"; then
          log_i "Hook already present in cameraserver PID=$CS_PID — skipping initial inject"
          echo 0 > "$CRASH_LOG"
          echo "active" > "$MODDIR/facegate/safety_state"
      else
          inject_hook "$CS_PID"
      fi
    fi

    # ── Post-inject verification with retries ────────────────────────────────────
    verify_hook() {
      local RETRIES=5
      local i
      for i in $(seq 1 $RETRIES); do
          local pid
          pid=$(find_camera_pid)
          if [ -n "$pid" ] && hook_present "$pid"; then
              log_i "VERIFIED: hookProxy.so active in cameraserver PID=$pid"
              echo 0 > "$CRASH_LOG"
              echo "active" > "$MODDIR/facegate/safety_state"
              return 0
          fi
          log_w "Verify attempt $i/$RETRIES: hook not confirmed"
          sleep 1
      done

      log_e "VERIFY FAILED: hook not present after $RETRIES attempts"
      CRASH_COUNT=$(cat "$CRASH_LOG" 2>/dev/null || echo 0)
      CRASH_COUNT=$((CRASH_COUNT + 1))
      echo "$CRASH_COUNT" > "$CRASH_LOG"

      if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
          log_e "SAFETY: reached $MAX_CRASHES failures — disabling module"
          touch "$MODDIR/disable"
          echo "disabled_crash_loop" > "$MODDIR/facegate/safety_state"
      else
          log_w "Crash retry $CRASH_COUNT / $MAX_CRASHES"
          echo "crash_retry_$CRASH_COUNT" > "$MODDIR/facegate/safety_state"
      fi
      return 1
    }

    [ -n "$CS_PID" ] && verify_hook

    # ── Background watchdog ───────────────────────────────────────────────────────
    # Re-injects whenever cameraserver restarts — no reboot or service restart needed.
    (
      sleep 30
      while true; do
          sleep "$WATCHDOG_INTERVAL_SEC"
          [ -f "$MODDIR/disable" ] && { log_w "WATCHDOG: module disabled — idle"; continue; }

          local_pid=$(find_camera_pid)
          if [ -z "$local_pid" ]; then
              continue  # cameraserver not running yet
          fi

          if hook_present "$local_pid"; then
              continue  # hook is live, nothing to do
          fi

          log_i "WATCHDOG: hook missing from cameraserver PID=$local_pid — re-injecting"
          CRASH_COUNT=$(cat "$CRASH_LOG" 2>/dev/null || echo 0)
          if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
              log_w "WATCHDOG: crash budget exhausted ($CRASH_COUNT) — idle"
              continue
          fi

          inject_hook "$local_pid"
          sleep 2

          if hook_present "$local_pid"; then
              log_i "WATCHDOG: re-injection SUCCESS for PID=$local_pid"
              echo 0 > "$CRASH_LOG"
              echo "active" > "$MODDIR/facegate/safety_state"
          else
              CRASH_COUNT=$((CRASH_COUNT + 1))
              echo "$CRASH_COUNT" > "$CRASH_LOG"
              log_e "WATCHDOG: re-injection FAILED (crash count=$CRASH_COUNT)"
              if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
                  touch "$MODDIR/disable"
                  echo "disabled_crash_loop" > "$MODDIR/facegate/safety_state"
                  log_e "WATCHDOG: crash-loop threshold reached — module disabled"
              else
                  echo "crash_retry_$CRASH_COUNT" > "$MODDIR/facegate/safety_state"
              fi
          fi
      done
    ) &

    log_i "=== service.sh complete (watchdog running in background) ==="
    log_i "=== Monitor: adb logcat -s 'amkush/*:V' ==="
    