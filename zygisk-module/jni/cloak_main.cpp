/* itsanon_cloak — [V34] native root-hide engine. Pure syscalls, no Kotlin,
 * no shell: enters the TARGET app's mount namespace and cleans it from the
 * inside so every native detector (mountinfo reads, stat/exec of su, dir
 * probes) sees a stock phone.
 *
 * usage: itsanon_cloak <pid>
 *
 * Kotlin only invokes this binary; the hiding itself lives here, at the
 * syscall layer — the only place it can be done reliably.
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DUMMY     "/data/local/tmp/.itsanon_dummy"
#define EMPTY_DIR "/data/local/tmp/.itsanon_empty"

static int has_ci(const char *hay, const char *needle) {
    if (!hay) return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return 1;
    }
    return 0;
}

/* unescape mountinfo octal sequences (\040 space, \011 tab, \012 nl, \134 \\) */
static void unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == '0' && r[2] && r[3]) {
            int v = (r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0');
            *w++ = (char)v; r += 4;
        } else *w++ = *r++;
    }
    *w = 0;
}

static int has_cs(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

static int looks_rooty(const char *mp) {
    /* [V73] Tab A8 (14.unisoc.1/.2 logs): the bare "ksu" needle matched
     * "checKSUm" — Samsung/Unisoc mount every ext4 partition with the
     * journal_checksum option, so the cloak flagged /mnt/vendor/efs,
     * /mnt/vendor/persist, /metadata and /omr as root artifacts, wasted
     * its umount attempts on them (EINVAL/ENOENT), the "E:" output forced
     * the script fallback (which lazy-detached those legit partitions!),
     * and verify counted them forever: "NOT cloaked, mounts=9, su=no".
     * KernelSU artifacts always carry "ksu" as a path component ("/ksu",
     * "ksu/") or as the exact uppercase "KSU" (tmpfs source) — checksum
     * cannot match any of those. "debug_ramdisk" is added because it is
     * APatch's working dir: its tmpfs mount is THE giveaway and no old
     * needle caught it. */
    return has_ci(mp, "magisk") || has_ci(mp, "/ksu") || has_ci(mp, "ksu/") ||
           has_cs(mp, "KSU") || has_ci(mp, "kernelsu") ||
           has_ci(mp, "apatch") || has_ci(mp, "debug_ramdisk") ||
           has_ci(mp, "/adb/") || has_ci(mp, "supersu") ||
           has_ci(mp, "/su/") || strcmp(mp, "/sbin/su") == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: itsanon_cloak <pid>\n"); return 2; }
    pid_t pid = (pid_t)atoi(argv[1]);
    if (pid <= 0) return 2;

    char ns[64];
    snprintf(ns, sizeof(ns), "/proc/%d/ns/mnt", (int)pid);
    int fd = open(ns, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open ns"); return 1; }
    if (setns(fd, CLONE_NEWNS) != 0) { perror("setns"); close(fd); return 1; }
    close(fd);

    /* 1) drop every root-tool mount from the target's namespace.
     * [V70] match the WHOLE mountinfo line, not just the mountpoint:
     * APatch/KernelSU overlay mounts sit on ordinary mountpoints (/system,
     * /vendor, ...) and only betray themselves in the root/source fields —
     * mountpoint-only matching unmounted nothing on APatch devices
     * (Tab A8: "ok" but 9 mounts survived). */
    int unmounted = 0, failed = 0, skipped = 0;
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (f) {
        char line[1024];
        char mps[4096]; size_t mps_len = 0; mps[0] = 0;
        while (fgets(line, sizeof(line), f)) {
            /* fields: id parent maj:min root MOUNTPOINT ... */
            char full[1024];
            strncpy(full, line, sizeof(full) - 1);
            full[sizeof(full) - 1] = 0;
            char *tok = strtok(line, " ");
            char *mp = NULL;
            for (int i = 0; tok && i < 5; i++) { if (i == 4) mp = tok; tok = strtok(NULL, " "); }
            if (!mp) continue;
            unescape(mp);
            if (looks_rooty(full)) {
                size_t l = strlen(mp);
                if (mps_len + l + 2 < sizeof(mps)) {
                    memcpy(mps + mps_len, mp, l + 1);
                    mps_len += l + 1;
                }
            }
        }
        fclose(f);
        /* unmount deepest-first not required with MNT_DETACH */
        char *p = mps;
        while (*p) {
            if (umount2(p, MNT_DETACH) == 0) unmounted++;
            else if (errno == ENOENT || errno == EINVAL) {
                /* [V73] EINVAL = not a mountpoint in this namespace,
                 * ENOENT = already detached (a parent mount went first).
                 * Neither is a cloak failure: pre-V73 these printed "E:"
                 * lines, the "ok" summary no longer came first, and
                 * CloakManager dropped to the script fallback every time. */
                skipped++;
            } else {
                failed++;
                printf("E: umount %s failed: %s\n", p, strerror(errno));
            }
            p += strlen(p) + 1;
        }
    }

    /* 2) bind-hide surviving su binaries (real files) with a dummy */
    mkdir(EMPTY_DIR, 0755);
    close(open(DUMMY, O_CREAT | O_RDWR, 0644));
    static const char *sus[] = {
        "/system/bin/su", "/system/xbin/su", "/sbin/su", "/vendor/bin/su",
        "/system/bin/.ext/.su", "/system/xbin/.ext/.su", "/system/sd/xbin/su",
        "/system/bin/failsafe/su", "/data/local/bin/su", "/data/local/xbin/su",
        NULL
    };
    int hidden = 0;
    for (int i = 0; sus[i]; i++) {
        struct stat st;
        if (stat(sus[i], &st) == 0) {
            if (mount(DUMMY, sus[i], NULL, MS_BIND, NULL) == 0) hidden++;
        }
    }
    /* 3) hide root-tool data dirs behind an empty dir */
    static const char *dirs[] = { "/data/adb", "/data/magisk", "/cache/magisk",
                                 "/data/adb/ap", "/data/adb/ksu", NULL };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            if (mount(EMPTY_DIR, dirs[i], NULL, MS_BIND, NULL) == 0) hidden++;
        }
    }

    printf("ok unmounted=%d hidden=%d failed=%d skipped=%d\n", unmounted, hidden, failed, skipped);
    return 0;
}
