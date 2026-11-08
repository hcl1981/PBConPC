// PBConPC measurement: a host-side sampling profiler for where PBConPC itself spends its time,
// for machines where perf is not allowed. PBCONPC_HOSTPROF=file samples the instruction pointer
// every 100 us of CPU time and writes "base <load address>" and one address per line at exit;
// tools/hostprof.py turns that into a list of functions. Linux on x86-64 only.
#if defined(__linux__) && defined(__x86_64__)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define MAX_SAMPLES (1u << 22)
static uintptr_t *samples;
static volatile unsigned nsamples;
static const char *out_path;

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = uc_;
    unsigned n = nsamples;
    if (n < MAX_SAMPLES) {
        samples[n] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        nsamples = n + 1;
    }
}

static void write_out(void)
{
    struct itimerval off = {0};
    setitimer(ITIMER_PROF, &off, NULL);
    FILE *f = fopen(out_path, "w");
    if (!f)
        return;
    Dl_info info;
    if (dladdr((void *)write_out, &info))
        fprintf(f, "base %lx\n", (unsigned long)(uintptr_t)info.dli_fbase);
    for (unsigned i = 0; i < nsamples; i++)
        fprintf(f, "%lx\n", (unsigned long)samples[i]);
    fclose(f);
    // the native code the JIT made, for tools/jitprof.py
    {
        extern void pbc_jit_dump(const char *path);
        char jp[4200];
        snprintf(jp, sizeof(jp), "%s.jit", out_path);
        pbc_jit_dump(jp);
    }
    // the process's memory map beside it, for naming library addresses
    char mp[4200];
    snprintf(mp, sizeof(mp), "%s.maps", out_path);
    FILE *in = fopen("/proc/self/maps", "r"), *mo = fopen(mp, "w");
    if (in && mo) {
        char line[512];
        while (fgets(line, sizeof(line), in))
            fputs(line, mo);
    }
    if (in)
        fclose(in);
    if (mo)
        fclose(mo);
}

// SIGUSR1 writes the samples so far, for a process that hangs (nsamples restarts from 0).
static void on_usr1(int sig)
{
    (void)sig;
    write_out();
    nsamples = 0;
    struct itimerval it = {{0, 100}, {0, 100}};
    setitimer(ITIMER_PROF, &it, NULL);
}

__attribute__((constructor)) static void hostprof_init(void)
{
    out_path = getenv("PBCONPC_HOSTPROF");
    if (!out_path || !*out_path)
        return;
    samples = calloc(MAX_SAMPLES, sizeof(*samples));
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 100}, {0, 100}};
    setitimer(ITIMER_PROF, &it, NULL);
    signal(SIGUSR1, on_usr1);
    atexit(write_out);
}
#endif
