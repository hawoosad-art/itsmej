#!/usr/bin/env python3
"""
patches/apply_andkitty_arm32_syscall.py
Fixes remote syscall execution on 32-bit ARM (Android 11 / TECNO etc.).

WHY: On 32-bit ARM, executing the remote syscall gadget with PTRACE_SINGLESTEP
fails: single-stepping a `svc #0` instruction in a shared-library text segment
(libc.so) delivers SIGTRAP/CODE=2 (TRAP_TRACE) instead of completing the syscall,
so the target process (cameraserver) dies before dlopen() completes:
    callSyscall(20): Process didn't stop after syscall!  SIG(Trap) CODE(2)
    PTRACE_SETREGS failed ... No such process

FIX: For ARM32, execute the syscall with PTRACE_SYSCALL (which stops at syscall
ENTRY and syscall EXIT) instead of PTRACE_SINGLESTEP. The kernel drives the
svc/return properly, so we can read the return value after the exit stop. This
is the standard, reliable way to inject a remote syscall on ARM32.

The patch:
  1. Adds `bool stepSyscall()` to KittyTraceMgr (uses PTRACE_SYSCALL + waits for
     the syscall-exit stop).
  2. In `_callSyscall`, under `#if defined(__arm__)`, replaces the
     "Single step to execute syscall" + "Wait for step" single-step path with a
     PTRACE_SYSCALL-based execution that stops at entry then exits, returning the
     syscall result. AArch64 keeps the existing single-step path.

Run as:
    python3 apply_andkitty_arm32_syscall.py <KittyTrace.cpp>
"""
import sys


def abort(msg: str):
    print(f"PATCH ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def apply(path: str):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    if "stepSyscall" in content:
        print("arm32 PTRACE_SYSCALL fix already present. Nothing to do.")
        return

    # ── Add stepSyscall() method after waitStep() ──────────────────────────
    # Anchor on the end of waitStep() (its closing brace followed by the next
    # member function declaration).
    anchor_waitstep = (
        "        waitpid(_pid, &status, 0);\n"
        "        if (!WIFSTOPPED(status))\n"
        "            return false;\n"
        "    }\n"
        "\n"
        "    return true;\n"
        "}\n"
    )
    if anchor_waitstep not in content:
        # Try a looser anchor: the final waitStep body.
        abort("waitStep() body not found — upstream changed structure")

    stepSyscall_method = (
        anchor_waitstep
        + "\n\n"
        + "/* ARM32 remote syscall execution via PTRACE_SYSCALL.\n"
        + " * On 32-bit ARM, PTRACE_SINGLESTEP over a `svc #0` in a shared library\n"
        + " * text segment (libc.so) fails with SIGTRAP/CODE=2, killing the target.\n"
        + " * PTRACE_SYSCALL stops at syscall entry and exit, letting the kernel\n"
        + " * drive the svc properly. Returns true once the syscall has exited. */\n"
        + "bool KittyTraceMgr::stepSyscall() const\n"
        + "{\n"
        + "    if (!_attached || _pid <= 0)\n"
        + "        return false;\n"
        + "\n"
        + "    // First PTRACE_SYSCALL: stop at syscall entry.\n"
        + "    errno = 0;\n"
        + "    if (ptrace(PTRACE_SYSCALL, _pid, nullptr, nullptr) == -1L)\n"
        + "    {\n"
        + "        KITTY_LOGE(\"PTRACE_SYSCALL(entry) failed for pid %d. \\\"%s\\\".\", _pid, strerror(errno));\n"
        + "        return false;\n"
        + "    }\n"
        + "    int status = 0;\n"
        + "    if (waitpid(_pid, &status, 0) == -1 || !WIFSTOPPED(status))\n"
        + "        return false;\n"
        + "\n"
        + "    // Second PTRACE_SYSCALL: resume and stop at syscall exit.\n"
        + "    errno = 0;\n"
        + "    if (ptrace(PTRACE_SYSCALL, _pid, nullptr, nullptr) == -1L)\n"
        + "    {\n"
        + "        KITTY_LOGE(\"PTRACE_SYSCALL(exit) failed for pid %d. \\\"%s\\\".\", _pid, strerror(errno));\n"
        + "        return false;\n"
        + "    }\n"
        + "    if (waitpid(_pid, &status, 0) == -1 || !WIFSTOPPED(status))\n"
        + "        return false;\n"
        + "\n"
        + "    return true;\n"
        + "}\n"
    )
    content = content.replace(anchor_waitstep, stepSyscall_method, 1)

    # ── In _callSyscall, replace the single-step block with ARM32 PTRACE_SYSCALL ──
    # The ARM32 path reads the return registers directly after stepSyscall() and
    # skips the single-step wait loop (the syscall already exited).
    anchor_step = (
        "    // Single step to execute syscall\n"
        "    if (!step())\n"
        "        return failure_return(KT_RP_CALL_STEP_FAILED);\n"
        "\n"
        "    // Wait for step\n"
        "    do\n"
        "    {\n"
    )
    if anchor_step not in content:
        abort("single-step block in _callSyscall not found — upstream changed")

    arm32_step = (
        "    // Execute the syscall.\n"
        "#if defined(__arm__)\n"
        "    // On 32-bit ARM, PTRACE_SINGLESTEP over `svc #0` in a shared-lib text\n"
        "    // segment fails (SIGTRAP/CODE=2 kills the target). Use PTRACE_SYSCALL\n"
        "    // which stops at syscall entry and exit so the kernel drives the svc.\n"
        "    if (!stepSyscall())\n"
        "        return failure_return(KT_RP_CALL_STEP_FAILED);\n"
        "    if (!getRegs(&return_regs))\n"
        "        return failure_return(KT_RP_CALL_REGS_FAILED);\n"
        "    goto arm32_syscall_done;\n"
        "#else\n"
        "    // Single step to execute syscall\n"
        "    if (!step())\n"
        "        return failure_return(KT_RP_CALL_STEP_FAILED);\n"
        "#endif\n"
        "\n"
        "    // Wait for step\n"
        "    do\n"
        "    {\n"
    )
    content = content.replace(anchor_step, arm32_step, 1)

    # ── Add the arm32_syscall_done: label before the _callSyscall success return ──
    # Use a context-specific anchor unique to _callSyscall (followed by the
    # _syscallGadget restore block) so we don't match _callFunction's result line.
    anchor_result = (
        "    } while (true);\n"
        "\n"
        "    kitty_rp_call_t result = {KT_RP_CALL_SUCCESS, {static_cast<intptr_t>(return_regs.KT_REG_RET)}};\n"
        "\n"
        "    if (_syscallGadget == 0)\n"
    )
    if anchor_result not in content:
        abort("_callSyscall success-return block not found — upstream changed")
    content = content.replace(
        anchor_result,
        "    } while (true);\n"
        "\n"
        "#if defined(__arm__)\n"
        "arm32_syscall_done:\n"
        "#endif\n"
        "    kitty_rp_call_t result = {KT_RP_CALL_SUCCESS, {static_cast<intptr_t>(return_regs.KT_REG_RET)}};\n"
        "\n"
        "    if (_syscallGadget == 0)\n",
        1,
    )

    # ── Declare stepSyscall() in the header ────────────────────────────────
    hdr_path = path.replace("KittyTrace.cpp", "KittyTrace.hpp")
    try:
        with open(hdr_path, "r", encoding="utf-8") as f:
            hdr = f.read()
        import re
        m = re.search(r"bool\s+step\s*\(\s*int\s+steps\s*=\s*1\s*\)\s*const\s*;", hdr)
        if not m:
            m = re.search(r"bool\s+step\s*\(\s*int\s+steps\s*\)\s*const\s*;", hdr)
        if m and "bool stepSyscall() const;" not in hdr:
            decl = "bool stepSyscall() const;\n    " + m.group(0)
            hdr = hdr.replace(m.group(0), decl, 1)
            with open(hdr_path, "w", encoding="utf-8") as f:
                f.write(hdr)
            print("Added stepSyscall() declaration to", hdr_path)
        elif "bool stepSyscall() const;" in hdr:
            print("stepSyscall() already declared in header")
        else:
            print("WARNING: step() declaration not found in header — add stepSyscall() manually")
    except FileNotFoundError:
        print("WARNING: KittyTrace.hpp not found next to KittyTrace.cpp — declaration not added")

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("arm32 PTRACE_SYSCALL fix applied to", path)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        abort("usage: apply_andkitty_arm32_syscall.py <KittyTrace.cpp>")
    apply(sys.argv[1])
