// DuckUSB service-side prototype. No app injection, kernel patches or code-page changes.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BASE "/data/adb/duckusb-service"
#define AUTH "/dev/socket/adbd"
struct record { int pid, listener, poller; uint64_t inode; };
static volatile sig_atomic_t stop_requested;
static void stopped(int signal_number) { (void)signal_number; stop_requested = 1; }

// Keep this root service outside the UI/ADB shell process group that launched it.
static int detach_launcher(void) {
    if (setsid() < 0 && errno != EPERM) return -1;
    FILE* file = fopen("/proc/self/cgroup", "r");
    if (!file) return -1;
    char line[512];
    bool unified = false, root = false;
    while (fgets(line, sizeof(line), file))
        if (strncmp(line, "0::", 3) == 0) { unified = true; root = strcmp(line, "0::/\n") == 0; }
    fclose(file);
    if (!unified || root) return 0;
    file = fopen("/sys/fs/cgroup/cgroup.procs", "w");
    if (!file) return -1;
    int result = fprintf(file, "%d\n", getpid()) > 0 ? 0 : -1;
    if (fclose(file) != 0) result = -1;
    return result;
}

static int daemon_pid(void) {
    DIR* directory = opendir("/proc");
    if (!directory) return -1;
    struct dirent* entry;
    int result = -1;
    while ((entry = readdir(directory))) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;
        char path[128], name[64] = {0};
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE* file = fopen(path, "r");
        if (!file) continue;
        char* got = fgets(name, sizeof(name), file);
        fclose(file);
        if (got && strcmp(name, "adbd\n") == 0) { result = pid; break; }
    }
    closedir(directory);
    return result;
}
static bool is_enabled(void) {
    FILE* file = fopen(BASE "/enabled", "r");
    if (!file) return false;
    int value = fgetc(file);
    fclose(file);
    return value == '1';
}
static void report(const char* state, int pid, int error) {
    FILE* file = fopen(BASE "/status", "w");
    if (file) { fprintf(file, "%s pid=%d errno=%d\n", state, pid, error); fclose(file); }
    printf("%s pid=%d errno=%d\n", state, pid, error);
    fflush(stdout);
}
static void save_record(const struct record* record) {
    FILE* file = fopen(BASE "/receipt", "w");
    if (file) { fprintf(file, "%d %d %d %" PRIu64 "\n", record->pid, record->listener, record->poller, record->inode); fclose(file); }
}
static bool read_record(struct record* record) {
    FILE* file = fopen(BASE "/receipt", "r");
    if (!file) return false;
    int count = fscanf(file, "%d %d %d %" SCNu64, &record->pid, &record->listener, &record->poller, &record->inode);
    fclose(file);
    return count == 4;
}
static uint64_t fd_inode(int pid, int fd) {
    char path[128], target[128];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, fd);
    ssize_t length = readlink(path, target, sizeof(target)-1);
    if (length <= 0) return 0;
    target[length] = 0;
    uint64_t inode = 0;
    return sscanf(target, "socket:[%" SCNu64 "]", &inode) == 1 ? inode : 0;
}
static bool find_listener(int pid, struct record* record) {
    char path[128], line[512], name[128];
    snprintf(path, sizeof(path), "/proc/%d/net/unix", pid);
    FILE* file = fopen(path, "r");
    if (!file) return false;
    uint64_t inode = 0, candidate;
    unsigned long flags;
    unsigned kind, state;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "%*s %*s %*s %lx %x %x %" SCNu64 " %127s", &flags, &kind, &state, &candidate, name) == 5 &&
            kind == SOCK_SEQPACKET && (flags & 0x10000) && strcmp(name, "@jdwp-control") == 0) { inode = candidate; break; }
    fclose(file);
    if (!inode) return false;
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR* directory = opendir(path);
    if (!directory) return false;
    int listener = -1, poller = -1;
    struct dirent* entry;
    while ((entry = readdir(directory))) {
        int fd = atoi(entry->d_name);
        if (fd_inode(pid, fd) == inode) { listener = fd; break; }
    }
    closedir(directory);
    if (listener < 0) return false;
    snprintf(path, sizeof(path), "/proc/%d/fdinfo", pid);
    directory = opendir(path);
    if (!directory) return false;
    while ((entry = readdir(directory))) {
        int fd = atoi(entry->d_name), watched;
        snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", pid, fd);
        file = fopen(path, "r");
        if (!file) continue;
        while (fgets(line, sizeof(line), file))
            if (sscanf(line, "tfd: %d", &watched) == 1 && watched == listener) { poller = fd; break; }
        fclose(file);
        if (poller >= 0) break;
    }
    closedir(directory);
    if (poller < 0) return false;
    *record = (struct record){pid, listener, poller, inode};
    return true;
}
static int change_listener(const struct record* record, bool restore) {
    if (record->listener < 6 || daemon_pid() != record->pid || fd_inode(record->pid, record->listener) != record->inode) { errno = ESRCH; return -1; }
    int process = syscall(__NR_pidfd_open, record->pid, 0);
    if (process < 0) return -1;
    int listener = syscall(__NR_pidfd_getfd, process, record->listener, 0);
    int poller = syscall(__NR_pidfd_getfd, process, record->poller, 0);
    int result = -1, error = errno;
    if (listener < 0 || poller < 0) goto done;
    if (listener != record->listener) {
        errno = 0;
        if (fcntl(record->listener, F_GETFD) != -1 || errno != EBADF) { error = EBUSY; goto done; }
        if (dup3(listener, record->listener, O_CLOEXEC) < 0) { error = errno; goto done; }
        close(listener);
        listener = record->listener;
    }
    struct sockaddr_un address;
    socklen_t length = sizeof(address);
    if (getsockname(listener, (struct sockaddr*)&address, &length) != 0 ||
        length != offsetof(struct sockaddr_un, sun_path) + 13 || address.sun_family != AF_UNIX ||
        memcmp(address.sun_path, "\0jdwp-control", 13) != 0) { error = EINVAL; goto done; }
    struct epoll_event event = {.events = EPOLLIN};
    event.data.fd = -1;
    if (restore) {
        if (listen(listener, 4) != 0) { error = errno; goto done; }
        if (epoll_ctl(poller, EPOLL_CTL_ADD, listener, &event) != 0 && errno != EEXIST) { error = errno; goto done; }
    } else {
        if (epoll_ctl(poller, EPOLL_CTL_DEL, listener, NULL) != 0) { error = errno; goto done; }
        if (shutdown(listener, SHUT_RDWR) != 0) {
            error = errno;
            epoll_ctl(poller, EPOLL_CTL_ADD, listener, &event);
            goto done;
        }
    }
    result = 0; error = 0;
done:
    if (listener >= 0) close(listener);
    if (poller >= 0) close(poller);
    close(process);
    errno = error;
    return result;
}
static bool socket_exists(const char* path) { struct stat info; return lstat(path, &info) == 0 && S_ISSOCK(info.st_mode); }
static int prepare_auth(const char* endpoint) {
    char directory[108];
    snprintf(directory, sizeof(directory), "%s", endpoint);
    char* slash = strrchr(directory, '/');
    if (!slash) { errno = EINVAL; return -1; }
    *slash = 0;
    if (mkdir(directory, 0750) != 0 && errno != EEXIST) return -1;
    if (chown(directory, 0, 1000) != 0 || chmod(directory, 0750) != 0) return -1;
    if (!socket_exists(AUTH)) return socket_exists(endpoint) ? 0 : -1;
    if (socket_exists(endpoint)) {
        char archive[192];
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        snprintf(archive, sizeof(archive), "%s.previous-%ld-%ld", endpoint, now.tv_sec, now.tv_nsec);
        if (rename(endpoint, archive) != 0) return -1;
    }
    return rename(AUTH, endpoint);
}
static int restore_auth(const char* endpoint, int expected_pid) {
    if (expected_pid <= 0 || daemon_pid() != expected_pid) return 0;
    if (socket_exists(AUTH)) return 0;
    if (!socket_exists(endpoint)) return 0;
    return rename(endpoint, AUTH);
}
int main(int argc, char** argv) {
    if (getuid() != 0 || argc != 3) return 2;
    const char* operation = argv[1], *endpoint = argv[2];
    if (strncmp(endpoint, "/dev/socket/.dusb-", 18) != 0 || strlen(endpoint) >= 100 ||
        strstr(endpoint, "..") || strcmp(endpoint + strlen(endpoint) - 5, "/adbd") != 0) return 3;
    int lock = open(BASE "/lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 4;
    signal(SIGTERM, stopped); signal(SIGINT, stopped);
    struct record record = {0};
    if (strcmp(operation, "restore") == 0) {
        bool receipt = read_record(&record);
        int auth = restore_auth(endpoint, record.pid);
        int error = auth == 0 ? 0 : errno;
        if (receipt && record.pid == daemon_pid() && change_listener(&record, true) != 0) error = errno;
        report(error == 0 ? "STOPPED" : "RESTORE_FAILED", daemon_pid(), error);
        return error == 0 ? 0 : 5;
    }
    if (strcmp(operation, "watch") != 0) return 6;
    if (detach_launcher() != 0) { report("DETACH_FAILED", daemon_pid(), errno); return 8; }
    int handled_pid = -1, error_pid = -1;
    while (!stop_requested && is_enabled()) {
        int pid = daemon_pid();
        if (pid > 0 && pid != handled_pid && pid != error_pid) {
            struct record next;
            if (find_listener(pid, &next)) {
                // Save enough information to restore even after an abnormal helper exit.
                save_record(&next);
                if (change_listener(&next, false) == 0) {
                    record = next;
                    if (prepare_auth(endpoint) == 0) { handled_pid = pid; report("ACTIVE", pid, 0); }
                    else { int error = errno; change_listener(&record, true); record.pid = 0; error_pid = pid; report("AUTH_MOVE_FAILED", pid, error); }
                } else if (error_pid != pid) { error_pid = pid; report("JDWP_ACCESS_FAILED", pid, errno); }
            }
        }
        sleep(1);
    }
    int auth = restore_auth(endpoint, record.pid);
    int error = auth == 0 ? 0 : errno;
    if (record.pid == daemon_pid() && change_listener(&record, true) != 0) error = errno;
    report(error == 0 ? "STOPPED" : "RESTORE_FAILED", daemon_pid(), error);
    close(lock);
    return error == 0 ? 0 : 7;
}
