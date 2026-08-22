# FaceGate Zygisk Module — Camera Hook Injection

## How the hook gets into `cameraserver`

`cameraserver` is a native daemon started directly by `init`, not forked
from Zygote — so Zygisk (which only sees Zygote-forked app processes) can
never reach it directly. Two approaches exist to get `libhookProxy.so`
loaded into it:

### Old approach: LD_PRELOAD (removed)

Set `resetprop wrap.cameraserver "LD_PRELOAD=/path/to/libhookProxy.so"` and
restart `cameraserver`, relying on `init`'s `wrap.<service>` support to
re-exec it with the preload env var set.

This was abandoned because:

- **`noatsecure` is unenforceable on Android 10+.** Wrapping a process whose
  domain differs from its exec's target domain requires the kernel to skip
  `AT_SECURE` clearing (`noatsecure`), which needs a
  `allow init cameraserver process { noatsecure }` sepolicy rule. AOSP ships
  a hard `neverallow init * { noatsecure }` since Android 10 — so this rule
  either silently doesn't apply (module fails) or requires patching the
  base policy, which isn't something a Magisk/KernelSU module can do.
- Every activation needed a **`cameraserver` restart** (frequently paired
  with a full device reboot in practice, since restarting a live system
  service mid-session is unreliable and it doesn't reliably keep other
  camera-stack clients happy).
- If the restart failed to pick up the wrapper (common on OEM ROMs that
  patch `init.rc` service definitions), there was no way to recover without
  another reboot.

### Current approach: ptrace injection

`service.sh` uses **[amkush_injector](injector/AndKittyInjector)** — a
vendored, patched build of
[AndKittyInjector](https://github.com/MJx0/AndKittyInjector) (ptrace-based
Android library injector, built on
[KittyMemoryEx](https://github.com/MJx0/KittyMemoryEx)) — to attach directly
to the **already-running** `cameraserver` process and remote-`dlopen()`
`libhookProxy.so` into it:

```sh
amkush_injector --pid <cameraserver-pid> --libs /path/to/libhookProxy.so --memfd
```

- **`--pid`** is an amkush-specific patch on top of upstream AndKittyInjector
  (which only supports `--package <name>`, since it's built for injecting
  into apps). Native daemons like `cameraserver` have no package name, so we
  added raw-PID targeting that bypasses package resolution entirely — see
  `injector/AndKittyInjector/src/main.cpp`.
- **`--memfd`** loads the library from an anonymous in-memory file descriptor
  instead of a path on disk, so `cameraserver`'s linker namespace never needs
  to resolve/execute a path under `/data/adb/...`.
- **`--hide` is intentionally never used.** It strips the library from
  `solist`/`dl_iterate_phdr` and randomizes its ELF header — which breaks
  ShadowHook's `shadowhook_init()`, which walks `solist` / dsyms the linker
  to hook `call_constructors`/`call_destructors`. A "hidden" `libhookProxy.so`
  is invisible to ShadowHook and hooking fails outright.

Once loaded, the dynamic linker runs `libhookProxy.so`'s normal ELF
constructor (`on_library_load()` in `jni/main.cpp`) exactly as it did under
LD_PRELOAD — the loading mechanism changed, not what happens after loading.

### Why this is strictly better

- **No sepolicy rule that's impossible to grant.** Only a `ptrace` allow rule
  is needed (`sepolicy.rule`), which is a normal, grantable capability.
- **No `cameraserver` restart, ever.** ptrace attaches to the live process.
- **Self-healing without a reboot.** `service.sh` runs a watchdog every 15s
  that checks `/proc/<pid>/maps` for the hook and **re-injects on the spot**
  if `cameraserver` restarted (crashed, OEM camera HAL bounce, etc.) and lost
  the hook. LD_PRELOAD had no equivalent recovery path short of another
  device reboot.
- **One reboot total**, and it's the same one every Magisk/KernelSU/APatch
  module needs anyway (to get `service.sh` mounted and running at boot) — not
  an FaceGate-specific extra step.

### Safety behavior (unchanged)

`service.sh` still tracks `facegate/crash_count` / `facegate/safety_state`
and disables itself (`touch disable`) after 3 failed injection attempts
within the cooldown window, whether the failure came from injection itself
or from the watchdog's re-injection attempts.

## Files

| File | Purpose |
|---|---|
| `service.sh` | Waits for `cameraserver`, ptrace-injects the hook, runs the re-injection watchdog |
| `post-fs-data.sh` | Early-boot manager detection + SELinux labeling |
| `sepolicy.rule` | Grants file access + `ptrace` (no `noatsecure`) |
| `customize.sh` | Magisk/KernelSU/APatch installer script; extracts `amkush_injector{32,64}` to `system/bin` |
| `jni/main.cpp` | `libhookProxy.so` — ELF constructor (ptrace-injected) + Zygisk fallback for app processes |
| `injector/AndKittyInjector`, `injector/KittyMemoryEx` | Vendored ptrace injector source, patched for `--pid` |
