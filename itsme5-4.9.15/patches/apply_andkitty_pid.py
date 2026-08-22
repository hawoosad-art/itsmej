#!/usr/bin/env python3
"""
patches/apply_andkitty_pid.py
Adds --pid <pid> direct-inject support to AndKittyInjector.

Designed for upstream MJx0/AndKittyInjector. Handles both:
  - Old format (pre-5.3.0): --package was required(); we add --pid + make --package optional.
  - New format (5.3.0+):    --pid is already in a proc_group; patch is a no-op.

Run as:
    python3 apply_andkitty_pid.py AndKittyInjector/src/main.cpp
"""
import sys

def abort(msg: str):
    print(f"PATCH ERROR: {msg}", file=sys.stderr)
    sys.exit(1)

def apply(path: str):
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()

    # ── Detect new upstream format (5.3.0+) ─────────────────────────────────
    # The new upstream already has --pid in a mutually exclusive proc_group.
    # If it's already there, no patching is needed — exit cleanly.
    NEW_FORMAT_MARKER = 'proc_group.add_argument("--pid")'
    if NEW_FORMAT_MARKER in content:
        print("Upstream already has --pid support (new proc_group format). No patch needed.")
        print("Patch skipped — nothing to do.")
        return

    # ── Old format patch (pre-5.3.0) ────────────────────────────────────────

    # ── Hunk 1: add --pid argument; make --package optional ─────────────────
    OLD1 = (
        'program.add_argument("--package")\n'
        '        .help("Target package name to inject into.")\n'
        '        .required()\n'
        '        .store_into(inj_cfg.package)\n'
        '        .metavar("<name>");'
    )
    NEW1 = (
        '// --pid: inject directly into a running PID (bypasses package resolution).\n'
        '    // Designed for system daemons like cameraserver that have no package name.\n'
        '    // Either --pid OR --package must be provided.\n'
        '    int direct_pid = 0;\n'
        '    program.add_argument("--pid")\n'
        '        .help("Inject directly into this PID (bypasses package name resolution).")\n'
        '        .store_into(direct_pid)\n'
        '        .metavar("<pid>");\n'
        '\n'
        '    program.add_argument("--package")\n'
        '        .help("Target package name to inject into (optional when --pid is used).")\n'
        '        .default_value(std::string(""))\n'
        '        .store_into(inj_cfg.package)\n'
        '        .metavar("<name>");'
    )
    if OLD1 not in content:
        abort("Hunk 1 (--package required) not found — already patched or wrong version?")
    content = content.replace(OLD1, NEW1, 1)
    print("Hunk 1: --pid argument added, --package made optional")

    # ── Hunk 2: add validation + pid logging after parse_args ───────────────
    OLD2 = (
        '    KITTY_LOGI("======== INJECTION ARGS ========");\n'
        '    KITTY_LOGI("package: %s", inj_cfg.package.c_str());'
    )
    NEW2 = (
        '    // Validate: --pid or --package must be given\n'
        '    if (direct_pid <= 0 && inj_cfg.package.empty()) {\n'
        '        std::cerr << "ERROR: Either --pid <pid> or --package <name> must be provided.\\n";\n'
        '        std::cerr << program;\n'
        '        return 1;\n'
        '    }\n'
        '\n'
        '    KITTY_LOGI("======== INJECTION ARGS ========");\n'
        '    KITTY_LOGI("pid: %d", direct_pid);\n'
        '    KITTY_LOGI("package: %s", inj_cfg.package.c_str());'
    )
    if OLD2 not in content:
        abort("Hunk 2 (INJECTION ARGS logging block) not found")
    content = content.replace(OLD2, NEW2, 1)
    print("Hunk 2: validation block + pid logging added")

    # ── Hunk 3: add direct_pid branch before launch/watch/package logic ─────
    OLD3 = '    if (inj_cfg.launch || inj_cfg.watch)'
    NEW3 = (
        '    if (direct_pid > 0)\n'
        '    {\n'
        '        // Direct PID mode: bypass all package resolution.\n'
        '        // Used for system processes like cameraserver with no package name.\n'
        '        KITTY_LOGI("Direct PID mode: targeting PID %d", direct_pid);\n'
        '\n'
        '        if (inj_cfg.delay > 0)\n'
        '            SLEEP_MICROS(inj_cfg.delay);\n'
        '\n'
        '        injection_ok = inject(direct_pid, libs, inj_cfg, &injected_libs_info);\n'
        '\n'
        '        if (!injection_ok)\n'
        '        {\n'
        '            KITTY_LOGE("Direct PID injection failed for PID %d.", direct_pid);\n'
        '            exit(1);\n'
        '        }\n'
        '    }\n'
        '    else if (inj_cfg.launch || inj_cfg.watch)'
    )
    if OLD3 not in content:
        abort("Hunk 3 (if launch||watch) not found")
    content = content.replace(OLD3, NEW3, 1)
    print("Hunk 3: direct_pid injection branch added")

    # ── Hunk 4: guard the package-based failure handler ─────────────────────
    OLD4 = (
        '    if (!injection_ok)\n'
        '    {\n'
        '        KITTY_LOGE("Injection failed.");\n'
        '        Utils::android_stop_app(inj_cfg.package);\n'
        '        int pid = KittyMemoryEx::getProcessID(inj_cfg.package);\n'
        '        if (pid > 0)\n'
        '        {\n'
        '            kill(pid, SIGKILL);\n'
        '        }\n'
        '        KITTY_LOGI("Killed target process.");\n'
        '        exit(1);\n'
        '    }'
    )
    NEW4 = (
        '    if (!injection_ok)\n'
        '    {\n'
        '        KITTY_LOGE("Injection failed.");\n'
        '        if (!inj_cfg.package.empty())\n'
        '        {\n'
        '            Utils::android_stop_app(inj_cfg.package);\n'
        '            int apid = KittyMemoryEx::getProcessID(inj_cfg.package);\n'
        '            if (apid > 0) { kill(apid, SIGKILL); }\n'
        '            KITTY_LOGI("Killed target process %s.", inj_cfg.package.c_str());\n'
        '        }\n'
        '        exit(1);\n'
        '    }'
    )
    if OLD4 not in content:
        abort("Hunk 4 (failure handler / kill process) not found — check upstream diff")
    content = content.replace(OLD4, NEW4, 1)
    print("Hunk 4: failure handler guarded for --pid mode")

    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

    print(f"Patch applied successfully to {path}")

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path/to/main.cpp>", file=sys.stderr)
        sys.exit(1)
    apply(sys.argv[1])
