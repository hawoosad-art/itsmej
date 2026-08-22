#!/usr/bin/env python3
"""
patches/apply_andkitty_filefd.py
Adds a "--filefd" dlopen mode to AndKittyInjector: instead of a memfd, the
target process opens a REAL file (via a remote openat syscall) and dlopens it
with android_dlopen_ext + ANDROID_DLEXT_USE_LIBRARY_FD.

WHY (Android 11 / 32-bit, e.g. TECNO):
  * memfd dlopen bypasses the linker-namespace filesystem restriction but its
    executable segment mmap is rejected with
        couldn't map "/memfd:<name> (deleted)" segment 1: Permission denied
    i.e. cameraserver can't exec-map a memfd it was handed (SELinux "execute"
    denial on the memfd inode), so the hook never loads.
  * file-path dlopen of /data/local/tmp on a 32-bit linker fails earlier with
    "library not found" (the bionic default namespace doesn't search that dir).
  * The file-fd mode is the PROVEN method from the 64-bit/Android-14 path: the
    library sits at a world-readable /data/local/tmp path, the target opens it
    with openat() and android_dlopen_ext() maps the REAL file. Bypasses the
    namespace search (we hand the loader an fd, no path lookup) AND the target
    execs a normal file it can be granted execute on (which works on-device).

Applies on TOP of the existing patches (pid + vdso_gadget + vdso + arm32).

Run as:
    python3 apply_andkitty_filefd.py <AndKittyInjector-src-root>
"""
import os
import sys


def abort(msg: str):
    print(f"PATCH ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def apply(root: str):
    syscall_hdr = os.path.join(root, "AndKittyInjector", "src", "Injector",
                               "KittyInjectorSyscall.hpp")
    injector_hpp = os.path.join(root, "AndKittyInjector", "src", "Injector",
                                "KittyInjector.hpp")
    kitty_cpp = os.path.join(root, "AndKittyInjector", "src", "Injector",
                             "KittyInjector.cpp")
    main_cpp = os.path.join(root, "AndKittyInjector", "src", "main.cpp")

    for f in (syscall_hdr, injector_hpp, kitty_cpp, main_cpp):
        if not os.path.exists(f):
            abort(f"file not found: {f}")

    # ── 1. syscall header: openat numbers per ABI ────────────────────────────
    hdr = open(syscall_hdr, "r", encoding="utf-8").read()
    if "#define syscall_openat_n" not in hdr:
        openat_by_abi = {
            "#define syscall_memfd_create_n 279\n": "#define syscall_openat_n 56\n",   # aarch64
            "#define syscall_memfd_create_n 385\n": "#define syscall_openat_n 322\n", # arm
            "#define syscall_memfd_create_n 356\n": "#define syscall_openat_n 295\n", # i386
            "#define syscall_memfd_create_n 319\n": "#define syscall_openat_n 257\n", # x86_64
        }
        for anchor, add in openat_by_abi.items():
            if anchor in hdr:
                hdr = hdr.replace(anchor, anchor + add)
            else:
                abort(f"openat anchor missing in syscall header: {anchor!r}")
        # ── ropenat() helper after rmemfd_create() ──────────────────────────
        rmemfd_anchor = "        return ret.result.val;\n    }\n\n    inline bool rmemfd_seal("
        if rmemfd_anchor not in hdr:
            abort("rmemfd_create() body not found in syscall header")
        ropenat = (
            "        return ret.result.val;\n    }\n\n"
            "    inline int ropenat(uintptr_t pathname, int flags)\n"
            "    {\n"
            "        if (!_kMgr || !_kMgr->isMemValid())\n"
            "            return 0;\n"
            "\n"
            "        // AT_FDCWD (-100) + openat(path, flags, 0)\n"
            "        auto ret = _kMgr->trace.callSyscall(syscall_openat_n, -100, pathname, flags, 0);\n"
            "        if (ret.result.val < 0)\n"
            "        {\n"
            "            _lastError = strerror(-ret.result.val);\n"
            "        }\n"
            "        return ret.result.val;\n"
            "    }\n\n    inline bool rmemfd_seal("
        )
        hdr = hdr.replace(rmemfd_anchor, ropenat, 1)
        open(syscall_hdr, "w", encoding="utf-8").write(hdr)
        print("syscall header: openat + ropenat added")
    else:
        print("syscall header: already patched")

    # ── 2. injector.hpp: cfg flag ───────────────────────────────────────────
    hpp = open(injector_hpp, "r", encoding="utf-8").read()
    if "filefd" not in hpp:
        hpp = hpp.replace("bool watch, launch, seize, bp, memfd, free, hide;",
                          "bool watch, launch, seize, bp, memfd, free, hide, filefd;")
        hpp = hpp.replace("memfd(false), free(false), hide(false), beforeEntryPoint",
                          "memfd(false), free(false), hide(false), filefd(false), beforeEntryPoint")
        open(injector_hpp, "w", encoding="utf-8").write(hpp)
        print("injector.hpp: --filefd cfg flag added")
    else:
        print("injector.hpp: already patched")

    # ── 3. main.cpp: --filefd arg + log ─────────────────────────────────────
    main = open(main_cpp, "r", encoding="utf-8").read()
    if "--filefd" not in main:
        anchor = 'program.add_argument("--memfd").help("Use memfd dlopen.").store_into(inj_cfg.memfd);'
        if anchor not in main:
            abort("--memfd arg line not found in main.cpp")
        main = main.replace(
            anchor,
            anchor + '\n'
            '    program.add_argument("--filefd").help("Use file-fd dlopen (bypass namespace).").store_into(inj_cfg.filefd);'
        )
        log_anchor = 'KITTY_LOGI("memfd: %d", inj_cfg.memfd ? 1 : 0);'
        if log_anchor in main:
            main = main.replace(
                log_anchor,
                log_anchor + '\n    KITTY_LOGI("filefd: %d", inj_cfg.filefd ? 1 : 0);'
            )
        open(main_cpp, "w", encoding="utf-8").write(main)
        print("main.cpp: --filefd arg added")
    else:
        print("main.cpp: already patched")

    # ── 4. KittyInjector.cpp: do_filefd_dlopen() + dispatch ─────────────────
    cpp = open(kitty_cpp, "r", encoding="utf-8").read()
    if "do_filefd_dlopen" not in cpp:
        # Insert do_filefd_dlopen() right before the do_memfd_dlopen lambda.
        anchor = "    auto do_memfd_dlopen = [&]() -> void {"
        if anchor not in cpp:
            abort("do_memfd_dlopen lambda not found in KittyInjector.cpp")
        new_block = (
            "    auto do_filefd_dlopen = [&]() -> void {\n"
            "        // Write the real library path into the target stack.\n"
            "        if (!_kMgr->writeMemStr(_rbuffer, elfFile.path()))\n"
            "        {\n"
            "            KITTY_LOGE(\"nativeInject: Failed to write lib path into stack!\");\n"
            "            return;\n"
            "        }\n"
            "\n"
            "        // Open the real file in the target (bypasses namespace search).\n"
            "        int rfd = _rsyscall.ropenat(_rbuffer, O_RDONLY | O_CLOEXEC);\n"
            "        if (rfd <= 0)\n"
            "        {\n"
            "            KITTY_LOGE(\"nativeInject: filefd remote openat failed, errno (\\\"%s\\\").\",\n"
            "                       _rsyscall.lastError().c_str());\n"
            "            return;\n"
            "        }\n"
            "        KITTY_LOGI(\"nativeInject: filefd openat -> fd=%d\", rfd);\n"
            "\n"
            "        android_dlextinfo extinfo = {};\n"
            "        extinfo.flags = ANDROID_DLEXT_USE_LIBRARY_FD;\n"
            "        extinfo.library_fd = rfd;\n"
            "\n"
            "        uintptr_t rdlextinfo = KT_ALIGN_UP(_rbuffer + elfFile.path().size() + 1, sizeof(uintptr_t));\n"
            "        if (!_kMgr->writeMem(rdlextinfo, &extinfo, sizeof(extinfo)))\n"
            "        {\n"
            "            KITTY_LOGE(\"nativeInject: Failed to write dlextinfo into stack!\");\n"
            "            return;\n"
            "        }\n"
            "\n"
            "        auto ret = _kMgr->trace.callFunctionFrom(_dl_caller, _rdlopen_ext, _rbuffer, _cfg.rtdl_flags, rdlextinfo);\n"
            "        if (ret.status != KT_RP_CALL_SUCCESS)\n"
            "        {\n"
            "            KITTY_LOGE(\"nativeInject: Failed to call dlopen_ext.\");\n"
            "            return;\n"
            "        }\n"
            "\n"
            "        info.dl_handle = ret.result.ptr;\n"
            "        if (info.dl_handle != 0)\n"
            "        {\n"
            "            info.soinfo = _kMgr->linkerScanner.findSoInfo(elfFile.path());\n"
            "            info.elf = _kMgr->elfScanner.findElf(elfFile.path(), EScanElfType::Native);\n"
            "            if (!info.elf.isValid())\n"
            "            {\n"
            "                info.elf = _kMgr->elfScanner.createWithSoInfo(info.soinfo);\n"
            "            }\n"
            "        }\n"
            "\n"
            "        if (!info.elf.isValid() && bCalldlerror)\n"
            "        {\n"
            "            *bCalldlerror = true;\n"
            "        }\n"
            "    };\n\n"
            + anchor
        )
        cpp = cpp.replace(anchor, new_block, 1)

        # Dispatch: memfd -> filefd -> legacy
        dispatch = (
            "    if (_cfg.memfd)\n"
            "    {\n"
            "        do_memfd_dlopen();\n"
            "    }\n"
            "    else\n"
            "    {\n"
            "        do_legacy_dlopen();\n"
            "    }\n"
        )
        new_dispatch = (
            "    if (_cfg.memfd)\n"
            "    {\n"
            "        do_memfd_dlopen();\n"
            "    }\n"
            "    else if (_cfg.filefd)\n"
            "    {\n"
            "        do_filefd_dlopen();\n"
            "    }\n"
            "    else\n"
            "    {\n"
            "        do_legacy_dlopen();\n"
            "    }\n"
        )
        if dispatch not in cpp:
            abort("nativeInject memfd dispatch block not found in KittyInjector.cpp")
        cpp = cpp.replace(dispatch, new_dispatch, 1)

        # Ensure O_RDONLY / O_CLOEXEC are available.
        if "#include <fcntl.h>" not in cpp:
            cpp = cpp.replace('#include "KittyInjector.hpp"',
                              '#include "KittyInjector.hpp"\n#include <fcntl.h>', 1)

        open(kitty_cpp, "w", encoding="utf-8").write(cpp)
        print("KittyInjector.cpp: do_filefd_dlopen() added + dispatch wired")
    else:
        print("KittyInjector.cpp: already patched")

    print("apply_andkitty_filefd.py OK")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: apply_andkitty_filefd.py <AndKittyInjector-src-root>", file=sys.stderr)
        sys.exit(1)
    apply(sys.argv[1])
