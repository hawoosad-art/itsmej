#!/usr/bin/env python3
"""
patches/apply_andkitty_vdso_gadget.py
Stops AndKittyInjector from choosing the vDSO as its remote syscall gadget.

WHY: On Qualcomm / Android 13 (and some ARM32), the vDSO is mapped non-writable
and single-stepping a syscall inside it produces SIGTRAP/CODE=2
("callSyscall(172): Process didn't stop after syscall!" -> the target process
dies before dlopen() completes). Upstream's gadget detector gives [vdso]
priority 1 in its regex and auto-selects it, so injection always fails on such
devices.

FIX: exclude [vdso] from the syscall-gadget scan and broaden the match to
file-backed executable segments (libc.so, /apex/*.so, /system/*.so) which ARE
writable and steppable. If no .so gadget is found, _syscallGadget stays 0 and
_callSyscall falls back to the POKEDATA target_pc_mem path (which the companion
apply_andkitty_vdso.py redirects away from the vdso as a safety net).

Run as:
    python3 apply_andkitty_vdso_gadget.py <KittyInjectorSyscall.hpp>
"""
import sys


def abort(msg: str):
    print(f"PATCH ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def apply(path: str):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    if "vdso excluded from syscall gadget" in content:
        print("vdso gadget exclusion already present. Nothing to do.")
        return

    # 1) getMapPriority: demote vdso so it is never preferred (defense in depth).
    old_prio = (
        "        auto getMapPriority = [](const std::string &path) -> int {\n"
        "            if (path == \"[vdso]\")\n"
        "                return 1;\n"
    )
    new_prio = (
        "        auto getMapPriority = [](const std::string &path) -> int {\n"
        "            // vdso excluded from syscall gadget: the vdso is non-writable and\n"
        "            // single-stepping it fails on Qualcomm/Android 13 (SIGTRAP/CODE=2),\n"
        "            // killing the target. Give it the lowest priority so a file-backed\n"
        "            // executable segment (libc.so / apex / system) is used instead.\n"
        "            if (path == \"[vdso]\")\n"
        "                return 100;\n"
    )
    if old_prio not in content:
        abort("getMapPriority vdso-priority hunk not found — upstream changed?")
    content = content.replace(old_prio, new_prio, 1)

    # 2) Regex filter: drop [vdso], match file-backed executable .so segments.
    old_regex = (
        '                                           "(^\\\\[vdso\\\\]$)|(^/system/.*\\\\.so$)");'
    )
    new_regex = (
        '                                           "(^/libc\\\\.so$)|(^/apex/.*\\\\.so$)|(^/system/.*\\\\.so$)");'
    )
    if old_regex not in content:
        abort("gadget regex hunk not found — upstream changed?")
    content = content.replace(old_regex, new_regex, 1)

    # 3) Comment marker (idempotency + documentation).
    marker = "    // vdso excluded from syscall gadget (QCOM/Android 13 fix)"
    if marker not in content:
        anchor = "        auto getMapPriority = [](const std::string &path) -> int {"
        content = content.replace(
            anchor, marker + "\n" + anchor, 1
        )

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("vdso gadget exclusion applied to", path)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        abort("usage: apply_andkitty_vdso_gadget.py <KittyInjectorSyscall.hpp>")
    apply(sys.argv[1])
