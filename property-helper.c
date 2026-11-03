// DuckUSB service.2: property-area updates only; USB HAL and adbd stay in control.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BASE
#define BASE "/data/adb/duckusb-properties"
#endif
#define N 4
static const char *keys[N] = {"persist.sys.usb.config", "init.svc.adbd", "sys.usb.state", "sys.usb.config"};
static const char *masks[N] = {"mtp", "stopped", "mtp", "mtp"};
static char original[N][PROP_VALUE_MAX], boot[80];
static bool changed[N];
static volatile sig_atomic_t stopping;
static const char *rp;
static void stop_signal(int n) { (void)n; stopping = 1; }
static void prop(const char *key, char *out) { out[0] = 0; __system_property_get(key, out); }
static bool text_read(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) { out[0] = 0; return false; }
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f); out[strcspn(out, "\r\n")] = 0;
    return ok;
}
static bool text_write(const char *path, const char *s) {
    FILE *f = fopen(path, "w"); if (!f) return false;
    bool ok = fprintf(f, "%s\n", s) > 0;
    return fclose(f) == 0 && ok;
}
static bool enabled(void) {
    char s[8]; return text_read(BASE "/enabled", s, sizeof(s)) && strcmp(s, "1") == 0;
}
static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static void status(const char *s) {
    static char last[120];
    if (strcmp(last, s)) { text_write(BASE "/status", s); snprintf(last, sizeof(last), "%s", s); }
}
static bool has_adb(const char *s) {
    const char *p = s;
    while (*p) {
        size_t n = strcspn(p, ",");
        if (n == 3 && strncmp(p, "adb", 3) == 0) return true;
        p += n; if (*p) ++p;
    }
    return false;
}
static bool adbd_alive(void) {
    DIR *d = opendir("/proc"); if (!d) return false;
    struct dirent *e; bool found = false;
    while ((e = readdir(d))) {
        if (atoi(e->d_name) <= 0) continue;
        char path[320], name[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        if (text_read(path, name, sizeof(name)) && strcmp(name, "adbd") == 0) { found = true; break; }
    }
    closedir(d); return found;
}
// Keep the new service outside the launching app's cgroup, as for the socket helper.
static bool detach_launcher(void) {
    if (setsid() < 0 && errno != EPERM) return false;
    FILE *f = fopen("/proc/self/cgroup", "r"); if (!f) return false;
    char line[512]; bool move = false;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "0::", 3) == 0 && strcmp(line, "0::/\n") != 0) move = true;
    fclose(f);
    if (!move) return true;
    char pid[32]; snprintf(pid, sizeof(pid), "%d", getpid());
    return text_write("/sys/fs/cgroup/cgroup.procs", pid);
}
static bool run_rp(const char *flag, const char *key, const char *value, char *output) {
    int pipefd[2] = {-1, -1};
    if (output && pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        if (output) { close(pipefd[0]); dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]); }
        if (value) execl(rp, rp, flag, key, value, (char *)NULL);
        else execl(rp, rp, flag, key, (char *)NULL);
        _exit(127);
    }
    if (output) close(pipefd[1]);
    if (pid < 0) { if (output) close(pipefd[0]); return false; }
    int result = 0; long long deadline = now_ms() + 3000;
    for (;;) {
        pid_t got = waitpid(pid, &result, WNOHANG);
        if (got == pid) break;
        if (got < 0 && errno != EINTR) { if (output) close(pipefd[0]); return false; }
        if (now_ms() >= deadline) { kill(pid, SIGKILL); waitpid(pid, &result, 0); result = -1; break; }
        usleep(10000);
    }
    if (output) {
        ssize_t n = read(pipefd[0], output, PROP_VALUE_MAX - 1);
        output[n > 0 ? n : 0] = 0;
        output[strcspn(output, "\r\n")] = 0;
        close(pipefd[0]);
    }
    return result != -1 && WIFEXITED(result) && WEXITSTATUS(result) == 0;
}
static bool save(void) {
    FILE *f = fopen(BASE "/restore-state", "w"); if (!f) return false;
    bool ok = fprintf(f, "%s\n", boot) > 0;
    for (int i = 0; i < N; ++i)
        if (fprintf(f, "%d %s\n", changed[i] ? 1 : 0, original[i]) < 0) ok = false;
    return fclose(f) == 0 && ok;
}
static bool load(void) {
    FILE *f = fopen(BASE "/restore-state", "r"); if (!f) return false;
    char line[160] = {0}; bool ok = fgets(line, sizeof(line), f) != NULL;
    line[strcspn(line, "\r\n")] = 0;
    ok = ok && strcmp(line, boot) == 0;
    for (int i = 0; ok && i < N; ++i) {
        ok = fgets(line, sizeof(line), f) != NULL && (line[0] == '0' || line[0] == '1') && line[1] == ' ';
        if (ok) {
            changed[i] = line[0] == '1';
            line[strcspn(line, "\r\n")] = 0;
            snprintf(original[i], sizeof(original[i]), "%.*s", PROP_VALUE_MAX - 1, line + 2);
        }
    }
    fclose(f); return ok;
}
static bool apply(int i, const char *current) {
    if (strcmp(current, masks[i]) == 0) return true;
    // Persist the receipt before changing memory, so a manual stop can recover it.
    snprintf(original[i], sizeof(original[i]), "%s", current);
    changed[i] = true;
    if (!save() || !run_rp("-n", keys[i], masks[i], NULL)) return false;
    char after[PROP_VALUE_MAX]; prop(keys[i], after);
    return strcmp(after, masks[i]) == 0;
}
static bool restore(void) {
    bool ok = true; char current[PROP_VALUE_MAX], vendor[PROP_VALUE_MAX];
    prop("vendor.usb.config", vendor);
    for (int i = 0; i < N; ++i) {
        if (!changed[i]) continue;
        prop(keys[i], current);
        if (strcmp(current, masks[i]) != 0) { changed[i] = false; continue; }
        const char *value = original[i];
        // Use current owners' state rather than a stale pre-reconnect snapshot.
        if (i == 1) value = adbd_alive() ? "running" : "stopped";
        if ((i == 2 || i == 3) && vendor[0]) value = vendor;
        if (!value[0] || !run_rp("-n", keys[i], value, NULL)) ok = false;
        else changed[i] = false;
    }
    if (!save()) ok = false;
    return ok;
}
static bool hal_selected(void) {
    char a[PROP_VALUE_MAX], b[PROP_VALUE_MAX];
    prop("sys.usb.configfs", a); prop("vendor.usb.use_gadget_hal", b);
    return !strcmp(a, "2") && !strcmp(b, "1");
}
static bool hal_route(void) {
    if (!hal_selected()) return false;
    // Restrict the new write to the exact forward-to-vendor route read from this ROM.
    FILE *f = fopen("/vendor/etc/init/hw/init.qcom.usb.rc", "r"); if (!f) return false;
    char line[512]; bool trigger = false, forward = false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line; while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "on ", 3) == 0)
            trigger = strstr(p, "property:sys.usb.config=* && property:sys.usb.configfs=2") != NULL;
        else if (trigger && strncmp(p, "setprop vendor.usb.config ${sys.usb.config}", strlen("setprop vendor.usb.config ${sys.usb.config}")) == 0) forward = true;
    }
    fclose(f); return forward;
}
static bool config_step(bool route, char *candidate, long long *since) {
    char cfg[PROP_VALUE_MAX], vendor[PROP_VALUE_MAX];
    prop("sys.usb.config", cfg); prop("vendor.usb.config", vendor);
    if (!route || !hal_selected()) {
        candidate[0] = 0; *since = 0;
        // A switch to legacy control needs its real input back immediately.
        if (changed[3]) {
            const char *value = vendor[0] ? vendor : original[3];
            if (!strcmp(cfg, masks[3]) && (!value[0] || !run_rp("-n", keys[3], value, NULL))) return false;
            changed[3] = false;
            return save();
        }
        return true;
    }
    if (!has_adb(cfg) || strcmp(cfg, vendor)) {
        candidate[0] = 0; *since = 0; return true;
    }
    if (strcmp(candidate, cfg)) { snprintf(candidate, PROP_VALUE_MAX, "%s", cfg); *since = now_ms(); return true; }
    if (now_ms() - *since < 2000) return true;
    // Allow init's actual sys->vendor handoff to complete before masking its read surface.
    char again[PROP_VALUE_MAX]; prop("sys.usb.config", again);
    if (strcmp(again, cfg)) { candidate[0] = 0; return true; }
    prop("vendor.usb.config", again);
    if (strcmp(again, cfg)) { candidate[0] = 0; return true; }
    return apply(3, cfg);
}
static void capture_legacy(void) {
    for (int i = 0; i < N; ++i) prop(keys[i], original[i]);
    // service.1 already masked these three keys; disk and the live owners recover them.
    char persisted[PROP_VALUE_MAX];
    if (run_rp("-P", keys[0], NULL, persisted) && persisted[0]) {
        snprintf(original[0], sizeof(original[0]), "%s", persisted);
        changed[0] = true;
    }
    changed[1] = true; changed[2] = true;
}
int main(int argc, char **argv) {
    if (getuid() != 0 || argc != 3 || argv[2][0] != '/') return 2;
    rp = argv[2];
    umask(0077);
    if (!text_read("/proc/sys/kernel/random/boot_id", boot, sizeof(boot))) return 3;
    int lock = open(BASE "/lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 4;
    bool previous = load();
    bool legacy_restore = !strcmp(argv[1], "restore-legacy");
    if (legacy_restore && !previous) { capture_legacy(); previous = true; }
    if (!strcmp(argv[1], "restore") || legacy_restore) {
        bool ok = !previous || restore(); status(ok ? "STOPPED" : "RESTORE_FAILED"); return ok ? 0 : 5;
    }
    if (strcmp(argv[1], "watch") || !enabled()) return 6;
    if (!detach_launcher()) { status("DETACH_FAILED"); return 7; }
    signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
    char pid[32]; snprintf(pid, sizeof(pid), "%d", getpid()); text_write(BASE "/pid", pid);
    if (!previous) capture_legacy();
    if (!save()) { status("SAVE_FAILED"); return 8; }
    bool route = hal_route(), ok = true;
    char candidate[PROP_VALUE_MAX] = {0}; long long since = 0;
    while (!stopping && enabled()) {
        for (int i = 0; i < 3; ++i) {
            char current[PROP_VALUE_MAX]; prop(keys[i], current);
            if (current[0] && !apply(i, current)) { ok = false; break; }
        }
        if (!ok || !config_step(route, candidate, &since)) { ok = false; break; }
        char cfg[PROP_VALUE_MAX]; prop(keys[3], cfg);
        bool hal = route && hal_selected();
        status(hal && !has_adb(cfg) ? "ACTIVE HAL" : hal ? "WAITING HAL SETTLE" : "ACTIVE BASE (HAL route unavailable)");
        sleep(1);
    }
    bool restored = restore();
    status(!restored ? "RESTORE_FAILED" : ok ? "STOPPED" : "WRITE_FAILED (restored)");
    close(lock); return restored && ok ? 0 : 9;
}
