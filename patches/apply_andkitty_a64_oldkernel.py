#!/usr/bin/env python3
"""
patches/apply_andkitty_a64_oldkernel.py
Fixes remote syscall execution on AArch64 devices with OLD kernels
(Xiaomi Mi A1, kernel 4.9 — custom Android 13 build).

WHY: Same failure family as the ARM32 fix, but on 64-bit:
PTRACE_SINGLESTEP over the poked `svc #0` stub is consumed by the exception
on old arm64 kernels — the stop reports SIGTRAP/CODE=2 (TRAP_TRACE) with the
PC still ON the stub, the syscall has NOT executed (verified: result reg 0),
so the "PC advanced" check fails:
    callSyscall(172): Process didn't stop after syscall!  SIG(Trap) CODE(2)
and the mismatch path then resumed the target with cont(WSTOPSIG) — RE-INJECTING
SIGTRAP, whose default action KILLED cameraserver:
    PTRACE_SETREGS failed ... "No such process"
Newer kernels (4.19+/5.x — Samsung A03s, TECNO BG6) advance the step on the
first try and never enter this path.

FIX (fallback only — working devices are untouched by construction):
  1. In _callSyscall's wait loop, when the stop is a TRAP with the PC still on
     the stub (the exact old-kernel signature), re-execute the syscall with
     PTRACE_SYSCALL via stepSyscall() (added by apply_andkitty_arm32_syscall.py
     — this patch MUST run after it) and take the normal success path.
  2. Never re-inject SIGTRAP/SIGSTOP when resuming after an unexpected stop
     (kittyReinjectSig helper on every cont(WSTOPSIG(status)) site) — a failed
     attempt can no longer kill the target.

Run as:
    python3 apply_andkitty_a64_oldkernel.py <KittyTrace.cpp>
"""
import sys


def abort(msg: str):
    print(f"PATCH ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def apply(path: str):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    if "a64_syscall_done" in content:
        print("a64 old-kernel svc fallback already present. Nothing to do.")
        return

    if "stepSyscall" not in content:
        abort("stepSyscall() not found — apply_andkitty_arm32_syscall.py must run FIRST")

    # ── 1. kittyReinjectSig helper after the include ──────────────────────
    anchor_inc = '#include "KittyTrace.hpp"\n'
    if content.count(anchor_inc) != 1:
        abort("include anchor not unique")
    content = content.replace(
        anchor_inc,
        anchor_inc
        + "\n"
        + "/* [a64-oldkernel] SIGTRAP/SIGSTOP must NEVER be re-injected into the\n"
        + " * tracee when resuming after an unexpected stop: SIGTRAP's default\n"
        + " * action killed cameraserver on Mi A1 (every \"didn't stop after\n"
        + " * syscall\" was followed by \"No such process\"). Trap/stop signals\n"
        + " * here are ptrace artifacts — suppress them; pass real signals. */\n"
        + "static inline int kittyReinjectSig(int status)\n"
        + "{\n"
        + "    int sig = WSTOPSIG(status);\n"
        + "    if (sig == SIGTRAP || sig == SIGSTOP || sig == SIGTSTP)\n"
        + "        return 0;\n"
        + "    return sig;\n"
        + "}\n",
        1,
    )

    # ── 2. suppress trap re-injection on every cont(WSTOPSIG(status)) ─────
    n = content.count("cont(WSTOPSIG(status))")
    if n < 1:
        abort("no cont(WSTOPSIG(status)) sites found — upstream changed")
    content = content.replace("cont(WSTOPSIG(status))", "cont(kittyReinjectSig(status))")
    print(f"kittyReinjectSig applied to {n} cont site(s)")

    # ── 3. fallback budget counter before the wait loop ───────────────────
    # After the arm32 patch the aarch64 single-step block ends with #endif.
    anchor_loop = "#endif\n\n    // Wait for step\n    do\n    {\n"
    if content.count(anchor_loop) != 1:
        abort("wait-loop anchor not unique — arm32 patch structure changed")
    content = content.replace(
        anchor_loop,
        "#endif\n"
        "\n"
        "    /* [a64-oldkernel] PTRACE_SYSCALL fallback budget (see wait loop).\n"
        "     * Declared without initializer + assigned separately so the ARM32\n"
        "     * goto above may legally jump past it (vacuous initialization). */\n"
        "    int a64_svc_fallback;\n"
        "    a64_svc_fallback = 0;\n"
        "\n"
        "    // Wait for step\n"
        "    do\n"
        "    {\n",
        1,
    )

    # ── 4. the fallback itself, before the mismatch error ─────────────────
    anchor_err = '        KITTY_LOGE("callSyscall(%d): Process didn\'t stop after syscall!", int(sysnr));\n'
    if content.count(anchor_err) != 1:
        abort("mismatch-error anchor not unique")
    content = content.replace(
        anchor_err,
        "        /* [a64-oldkernel] Mi A1 (kernel 4.9): PTRACE_SINGLESTEP over the\n"
        "         * svc stub is consumed by the exception — TRAP_TRACE with the PC\n"
        "         * still ON the stub; the syscall has NOT executed. Re-execute it\n"
        "         * with PTRACE_SYSCALL (kernel-driven, reliable on every kernel —\n"
        "         * same mechanism as the ARM32 fix). Only this exact signature\n"
        "         * enters the fallback, so devices whose single-step advances\n"
        "         * (4.19+/5.x: Samsung, TECNO) never change behavior. */\n"
        "        if (return_regs.KT_REG_PC == tmp_regs.KT_REG_PC && a64_svc_fallback < 2)\n"
        "        {\n"
        "            siginfo_t si_pre = {};\n"
        "            getSignalInfo(&si_pre);\n"
        "            if (si_pre.si_signo == SIGTRAP)\n"
        "            {\n"
        "                a64_svc_fallback++;\n"
        '                KITTY_LOGI("callSyscall(%d): step-trap on stub PC (old-kernel SS) — PTRACE_SYSCALL fallback (%d/2)",\n'
        "                           int(sysnr), a64_svc_fallback);\n"
        "                if (setRegs(&tmp_regs) && stepSyscall() && getRegs(&return_regs))\n"
        "                {\n"
        '                    KITTY_LOGI("callSyscall(%d): PTRACE_SYSCALL fallback OK, ret=%p",\n'
        '                               int(sysnr), (void *)return_regs.KT_REG_RET);\n'
        "                    goto a64_syscall_done;\n"
        "                }\n"
        '                KITTY_LOGE("callSyscall(%d): PTRACE_SYSCALL fallback failed.", int(sysnr));\n'
        "            }\n"
        "        }\n"
        "\n"
        + anchor_err,
        1,
    )

    # ── 5. success label (unconditional so both arch builds compile) ──────
    anchor_done = (
        "#if defined(__arm__)\n"
        "arm32_syscall_done:\n"
        "#endif\n"
        "    kitty_rp_call_t result = {KT_RP_CALL_SUCCESS, {static_cast<intptr_t>(return_regs.KT_REG_RET)}};\n"
    )
    if content.count(anchor_done) != 1:
        abort("success-label anchor not unique — arm32 patch structure changed")
    content = content.replace(
        anchor_done,
        "#if defined(__arm__)\n"
        "arm32_syscall_done:\n"
        "#endif\n"
        "a64_syscall_done:\n"
        "    kitty_rp_call_t result = {KT_RP_CALL_SUCCESS, {static_cast<intptr_t>(return_regs.KT_REG_RET)}};\n",
        1,
    )

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("a64 old-kernel svc fallback applied to", path)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        abort("usage: apply_andkitty_a64_oldkernel.py <KittyTrace.cpp>")
    apply(sys.argv[1])
