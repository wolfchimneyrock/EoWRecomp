// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include "cpu_check.h"
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
// (the GPU imports the heap with VK_EXT_external_memory_host here)
bool plat_heap_gpu_backing(uint64_t addr, uint64_t size, uint64_t old_size) { (void)addr, (void)size, (void)old_size; return false; }
void *plat_heap_gpu_handle(int i, uint64_t *size, int *gen) { (void)i, *size = 0, *gen = 0; return NULL; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// Started from a desktop (the AppImage) there is no terminal: stdout / stderr go to eowrecomp.log in the data folder
// (runtime/paths.h; rt_paths_init runs later, so the same rule is applied here). Processes started by this one (the
// start screen re-executing itself for the game) append to the same log.
static char g_log_path[1024];

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    char dir[900];
    const char *env = getenv("EOWRECOMP_DATA_ROOT"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode)) return;  // dev mode: wherever stderr goes already
    else if (xdg && *xdg == '/') snprintf(dir, sizeof dir, "%s/eowrecomp", xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/eowrecomp", home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/eowrecomp.log", dir);
    bool append = getenv("EOWRECOMP_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("EOWRECOMP_LOG_APPEND", "1", 1);
}

#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    if (cpu_has_x86_64_v3()) return;
    fprintf(stderr, "FATAL: EoWRecomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    log_to_file_without_terminal();
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe eowrecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, "EoWRecomp stopped because of an error.\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EoWRecomp", text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (eowrecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }

// ---- big.LITTLE / hybrid CPUs: the frame's critical threads on the big cores (from LAS Recomp #533)
// The scheduler places a thread by its recent load, so the game or render thread, idle for part of each frame, often
// lands on a little core: on an RK3588 (4x Cortex-A55 + 4x A76) LAS Recomp's game thread ran on an A55 most of the time
// and frames missed their 33 ms. The game's guest threads (its main and render threads among them), the vsync thread
// and whatever they start go on the big cores; the shader translator and the build workers on all cores
// (plat_thread_default_core: a thread inherits its creator's affinity). A core's class: its cpu_capacity (arm64, from
// the device tree), else its highest frequency. "Big" = every class above the lowest, within the process affinity.
// EOWRECOMP_PCORE=0: off (the OS's default scheduling). EOWRECOMP_PCORE_GUEST=0: only the game's main thread, the
// submitting (render) thread and vsync on the big cores, the other guest threads anywhere.
static pthread_once_t pcore_once = PTHREAD_ONCE_INIT;
static cpu_set_t pcore_set, pcore_all;  // the big cores; the process's own cores
static int pcore_n;  // 0: no selection (off, not hybrid, or the affinity allows only one class)

static long cpu_sysfs_long(int cpu, const char *file) {
    char p[96], b[32];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/%s", cpu, file);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    long v = fgets(b, sizeof b, f) ? atol(b) : -1;
    fclose(f);
    return v;
}

#if !defined(__aarch64__) && !defined(__arm__)
// "0-7,16-23" (a sysfs cpu list) into a set; the number of cpus, or -1 when the file is missing / empty
static int cpulist_read(const char *path, cpu_set_t *set) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char b[256];
    bool ok = fgets(b, sizeof b, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    CPU_ZERO(set);
    int n = 0;
    for (char *p = b; *p && *p != '\n';) {
        char *q;
        long lo = strtol(p, &q, 10), hi = lo;
        if (q == p) break;
        if (*q == '-') hi = strtol(q + 1, &q, 10);
        for (long c = lo; c <= hi && c < CPU_SETSIZE; c++) CPU_SET((int)c, set), n++;
        p = *q == ',' ? q + 1 : q;
    }
    return n ? n : -1;
}
#endif

static void pcore_log_set(const char *how, int ncpu) {
    char list[256] = "";
    size_t n = 0;
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &pcore_set) && n < sizeof list - 8) n += (size_t)snprintf(list + n, sizeof list - n, "%s%d", n ? "," : "", c);
    fprintf(stderr, "[pcore] big.LITTLE / hybrid CPU (%s): game threads on cores %s (%d of %d)\n", how, list, pcore_n, ncpu);
}

static void pcore_init(void) {
    const char *e = getenv("EOWRECOMP_PCORE");
    if (e && *e && atoi(e) == 0) { fprintf(stderr, "[pcore] off (EOWRECOMP_PCORE=0): default scheduling\n"); return; }
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof allowed, &allowed) != 0) return;
    pcore_all = allowed;
    int ncpu = CPU_COUNT(&allowed);
    char how[96];
#if defined(__aarch64__) || defined(__arm__)
    // arm64: each core's capacity from the device tree; big = every class above the lowest
    long cap[CPU_SETSIZE], lo = -1, hi = -1;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        cap[c] = CPU_ISSET(c, &allowed) ? cpu_sysfs_long(c, "cpu_capacity") : -1;
        if (cap[c] <= 0) continue;
        if (lo < 0 || cap[c] < lo) lo = cap[c];
        if (cap[c] > hi) hi = cap[c];
    }
    if (lo > 0 && hi > lo) {
        CPU_ZERO(&pcore_set);
        for (int c = 0; c < CPU_SETSIZE; c++)
            if (cap[c] > lo) CPU_SET(c, &pcore_set), pcore_n++;
        snprintf(how, sizeof how, "cpu_capacity %ld..%ld", lo, hi);
        pcore_log_set(how, ncpu);
        return;
    }
    if (lo > 0) { fprintf(stderr, "[pcore] %d cores of one capacity: default scheduling\n", ncpu); return; }
#else
    // Intel hybrid: the kernel lists the P and E cores (these files exist only on a hybrid CPU)
    cpu_set_t pc, ec;
    if (cpulist_read("/sys/devices/cpu_core/cpus", &pc) > 0 && cpulist_read("/sys/devices/cpu_atom/cpus", &ec) > 0) {
        CPU_AND(&pcore_set, &pc, &allowed);
        CPU_AND(&ec, &ec, &allowed);
        if (CPU_COUNT(&pcore_set) && CPU_COUNT(&ec)) {
            pcore_n = CPU_COUNT(&pcore_set);
            pcore_log_set("cpu_core / cpu_atom", ncpu);
            return;
        }
        fprintf(stderr, "[pcore] hybrid CPU, but the affinity allows one kind only: default scheduling\n");
        return;
    }
#endif
    // Otherwise each core's highest frequency, and only a large gap counts: a desktop's favoured cores (Turbo Boost Max
    // 3.0, some Ryzen frequency drivers) report a few percent more than the rest, and pinning the game to those one or
    // two would be a big regression. Big and little when the slowest is under 80 % of the fastest.
    long f[CPU_SETSIZE], flo = -1, fhi = -1;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        f[c] = CPU_ISSET(c, &allowed) ? cpu_sysfs_long(c, "cpufreq/cpuinfo_max_freq") : -1;
        if (f[c] <= 0) continue;
        if (flo < 0 || f[c] < flo) flo = f[c];
        if (f[c] > fhi) fhi = f[c];
    }
    if (flo <= 0 || flo * 5 >= fhi * 4) {
        fprintf(stderr, "[pcore] %d cores of one class (highest frequency %ld..%ld kHz): default scheduling\n", ncpu, flo, fhi);
        return;
    }
    CPU_ZERO(&pcore_set);
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (f[c] > 0 && f[c] * 5 >= fhi * 4) CPU_SET(c, &pcore_set), pcore_n++;
    snprintf(how, sizeof how, "highest frequency %ld..%ld kHz", flo, fhi);
    pcore_log_set(how, ncpu);
}

void plat_children_elsewhere(bool begin) { (void)begin; }  // (phones only: runtime/platform/android.c)

void plat_thread_default_core(void) {
    pthread_once(&pcore_once, pcore_init);
    if (pcore_n) pthread_setaffinity_np(pthread_self(), sizeof pcore_all, &pcore_all);
}

void plat_thread_core(int role) {
    pthread_once(&pcore_once, pcore_init);
    if (!pcore_n || role < 0 || role > PLAT_CORE_VSYNC) return;
    bool big = true;
    if (role == PLAT_CORE_OTHER) {  // (a guest thread)
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("EOWRECOMP_PCORE_GUEST");
            on = !e || !*e || atoi(e) != 0;
            fprintf(stderr, "[pcore] guest threads: %s\n", on ? "big cores" : "all cores (EOWRECOMP_PCORE_GUEST=0)");
        }
        big = on;
    }
    bool ok = pthread_setaffinity_np(pthread_self(), sizeof (big ? pcore_set : pcore_all), big ? &pcore_set : &pcore_all) == 0;
    static _Atomic int logged;  // (once per role: a reboot's new threads apply the same, quietly)
    static const char *const names[] = {"game", "render", "guest", "vsync"};
    if (role == PLAT_CORE_OTHER || (atomic_fetch_or(&logged, 1 << role) & (1 << role))) return;
    fprintf(stderr, "[pcore] %s thread: %s\n", names[role], ok ? "big cores" : "affinity refused (default scheduling)");
}
void plat_frame_work(uint64_t busy_ns, uint64_t target_ns) { (void)busy_ns, (void)target_ns; }
void plat_thread_name(const char *name) { pthread_setname_np(pthread_self(), name); }

// Freeze report / menu Freeze log: Windows only for now (runtime/platform.h)
void plat_thread_backtrace(pthread_t thread) { (void)thread; }
void plat_log_to_file(void) {}
