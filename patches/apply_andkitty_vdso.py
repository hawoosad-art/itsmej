#!/usr/bin/env python3
"""
patches/apply_andkitty_vdso.py
Adds a vDSO PC-redirect fix to AndKittyInjector's KittyMemoryEx/KittyTrace.cpp.

WHY: On Qualcomm / Android 13 (and ARM32), the remote syscall injection inside
the ptrace-based injector writes the syscall opcode into the target's current PC.
When that PC happens to be inside [vdso] (a non-writable kernel-backed page),
PTRACE_POKEDATA silently fails and the single-step hits whatever is in the vdso
(often a BKPT/svc stub) -> SIGTRAP/CODE=2 -> the target (cameraserver) dies
before dlopen() completes, so injection never finishes.

FIX: when the injection PC is inside [vdso], redirect it to a file-backed
executable region (e.g. libc.so text). The syscall number/args live in registers,
so the injection site address does not matter. Original registers are restored
afterwards, so the process resumes from its real PC.

Run as:
    python3 apply_andkitty_vdso.py <path-to-KittyTrace.cpp>
"""
import sys


def abort(msg: str):
    print(f"PATCH ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def apply(path: str):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    # Idempotency guard.
    if "vDSO PC redirect" in content:
        print("vDSO redirect fix already present. Nothing to do.")
        return

    # Anchor: the else-branch in _callSyscall where we inject the redirect.
    # We must insert our block right after "else {" and before the
    # executable-region check.
    anchor = (
        "    else\n"
        "    {\n"
        "        if (!KittyMemoryEx::getAddressMap(_pid, target_pc_mem).executable)"
    )
    if anchor not in content:
        abort("anchor 'else { if (!KittyMemoryEx::getAddressMap...' not found — "
              "upstream may have changed structure")

    insertion = (
        "    else\n"
        "    {\n"
        "        // vDSO redirect: if the current PC is inside [vdso], PTRACE_POKEDATA\n"
        "        // silently fails because the vDSO is backed by a special non-writable\n"
        "        // kernel page. Leaving the original bytes intact means single-step hits\n"
        "        // whatever the vDSO has there (often a BKPT/svc stub), causing\n"
        "        // SIGTRAP/CODE=2 which kills the process when delivered.\n"
        "        // Fix: find a file-backed executable region (e.g. libc.so text) and\n"
        "        // redirect the syscall injection there. The syscall args/number are in\n"
        "        // registers, so the injection site address does not matter. Original\n"
        "        // registers are restored afterwards, so the process resumes from its\n"
        "        // real PC. Affects ARM32 (API 28-29) and AArch64 (Android 13, API 33).\n"
        "#if defined(__arm__) || defined(__aarch64__)\n"
        "        {\n"
        "            auto pcMap = KittyMemoryEx::getAddressMap(_pid, target_pc_mem);\n"
        "            if (pcMap.isValid() && pcMap.pathname == \"[vdso]\")\n"
        "            {\n"
        "                auto allMaps = KittyMemoryEx::getAllMaps(_pid);\n"
        "                for (const auto &m : allMaps)\n"
        "                {\n"
        "                    // Need a file-backed executable segment with room for 4 bytes.\n"
        "                    if (!m.executable || m.pathname.empty() || m.pathname[0] != '/')\n"
        "                        continue;\n"
        "                    if (m.endAddress < m.startAddress + 8)\n"
        "                        continue;\n"
        "                    // Use an address 4 bytes into the segment — 4-byte aligned.\n"
        "                    uintptr_t altPc = (m.startAddress + 4) & ~3u;\n"
        "                    if (altPc + 4 > m.endAddress)\n"
        "                        continue;\n"
        "                    KITTY_LOGI(\"callSyscall(%d): vDSO PC redirect %p -> %p (%s)\",\n"
        "                               int(sysnr), (void *)target_pc_mem, (void *)altPc,\n"
        "                               m.pathname.c_str());\n"
        "                    target_pc_mem      = altPc;\n"
        "                    tmp_regs.KT_REG_PC = altPc;\n"
        "#if defined(__arm__)\n"
        "                    tmp_regs.KT_REG_CPSR &= ~KT_CPSR_T_MASK; // force ARM mode (4-byte svc)\n"
        "#endif\n"
        "                    break;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "#endif\n"
        "\n"
        "        if (!KittyMemoryEx::getAddressMap(_pid, target_pc_mem).executable)"
    )

    content = content.replace(anchor, insertion, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("vDSO redirect fix applied to", path)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        abort("usage: apply_andkitty_vdso.py <KittyTrace.cpp>")
    apply(sys.argv[1])
