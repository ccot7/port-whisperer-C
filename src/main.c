/*
 * port-whisperer — bare-metal C port of LarsenCundric/port-whisperer
 * Supports Linux (Fedora/Wayland) and macOS.
 *
 * Build deps: none (pure POSIX + platform syscalls)
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>

// If for some reason PATH_MAX isn't defined, we set it to the Linux standard
#ifndef PATH_MAX
    #define PATH_MAX 4096
#endif

/* ── platform (defined by Makefile via -DPLATFORM_MACOS / -DPLATFORM_LINUX) ── */
#if !defined(PLATFORM_MACOS) && !defined(PLATFORM_LINUX)
#  ifdef __APPLE__
#    define PLATFORM_MACOS
#  else
#    define PLATFORM_LINUX
#  endif
#endif

/* ── ANSI colours ─────────────────────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_RED     "\033[31m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"
#define COL_BLUE    "\033[34m"
#define COL_WHITE   "\033[37m"
#define COL_BGREEN  "\033[92m"
#define COL_BYELLOW "\033[93m"
#define COL_BRED    "\033[91m"

/* ── box-drawing ──────────────────────────────────────────────── */
#define BOX_TL "┌"
#define BOX_TR "┐"
#define BOX_BL "└"
#define BOX_BR "┘"
#define BOX_H  "─"
#define BOX_V  "│"
#define BOX_TM "┬"
#define BOX_BM "┴"
#define BOX_ML "├"
#define BOX_MR "┤"
#define BOX_MM "┼"
#define DOT_GREEN  COL_BGREEN  "●" COL_RESET
#define DOT_YELLOW COL_BYELLOW "●" COL_RESET
#define DOT_RED    COL_BRED    "●" COL_RESET

/* ── limits ───────────────────────────────────────────────────── */
#define MAX_PORTS   512
#define MAX_PROCS   512
#define BUF         4096
#define SMBUF       256

/* ── data types ───────────────────────────────────────────────── */
typedef struct {
    int    port;
    char   proto[8];
    pid_t  pid;
    char   process[SMBUF];
    char   cmdline[BUF];
    char   cwd[BUF];
    char   project[SMBUF];
    char   framework[SMBUF];
    long   uptime_s;
    long   mem_kb;
    float  cpu_pct;
    char   status[SMBUF];
    int    is_docker;
    char   container[SMBUF];
} PortEntry;

typedef struct {
    pid_t  pid;
    char   process[SMBUF];
    char   cmdline[BUF];
    char   cwd[BUF];
    char   project[SMBUF];
    char   framework[SMBUF];
    long   uptime_s;
    long   mem_kb;
    float  cpu_pct;
    char   status[SMBUF];
    int    is_docker_group;
    int    docker_count;
} ProcEntry;

/* ── globals ──────────────────────────────────────────────────── */
static PortEntry g_ports[MAX_PORTS];
static int       g_nports = 0;
static ProcEntry g_procs[MAX_PROCS];
static int       g_nprocs = 0;
static int       g_show_all = 0;

/* ════════════════════════════════════════════════════════════════
   UTILITY
   ════════════════════════════════════════════════════════════════ */

static void str_trim(char *s) {
    char *p = s + strlen(s) - 1;
    while (p >= s && isspace((unsigned char)*p)) *p-- = '\0';
    p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int str_contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

/* Run a shell command and return its output (caller frees). Returns NULL on fail. */
static char *run_cmd(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    char tmp[4096];
    while (fgets(tmp, sizeof(tmp), fp)) {
        size_t n = strlen(tmp);
        if (len + n + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

/* Format seconds as "Xd Yh" / "Yh Zm" / "Zm Ws" */
static void fmt_uptime(long secs, char *out, size_t sz) {
    if (secs < 0) { snprintf(out, sz, "-"); return; }
    long d = secs / 86400, h = (secs % 86400) / 3600,
         m = (secs % 3600) / 60,  s = secs % 60;
    if (d > 0)      snprintf(out, sz, "%ldd %ldh", d, h);
    else if (h > 0) snprintf(out, sz, "%ldh %ldm", h, m);
    else if (m > 0) snprintf(out, sz, "%ldm %lds", m, s);
    else            snprintf(out, sz, "%lds",       s);
}

/* Format KB as "X.X MB" / "X KB" */
static void fmt_mem(long kb, char *out, size_t sz) {
    if (kb <= 0) { snprintf(out, sz, "-"); return; }
    if (kb >= 1024) snprintf(out, sz, "%.1f MB", kb / 1024.0);
    else            snprintf(out, sz, "%ld KB", kb);
}

/*
 * Display width of a string: strips ANSI escape sequences and counts
 * Unicode codepoints (each codepoint = 1 column for our purposes).
 * This is what must be used for ALL column width calculations so that
 * measurement and padding are consistent.
 */
static int display_width(const char *s) {
    int w = 0, esc = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\033')          { esc = 1; continue; }
        if (esc)                   { if (*p == 'm') esc = 0; continue; }
        if ((*p & 0xC0) == 0x80)  continue; /* UTF-8 continuation byte */
        w++;
    }
    return w;
}

/* ════════════════════════════════════════════════════════════════
   FRAMEWORK DETECTION
   ════════════════════════════════════════════════════════════════ */

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int read_file(const char *path, char *buf, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sz - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 1;
}

static void detect_framework(const char *cwd, const char *cmdline,
                              char *fw_out, size_t fw_sz,
                              char *proj_out, size_t proj_sz) {
    const char *slash = strrchr(cwd, '/');
    if (slash && *(slash+1))
        snprintf(proj_out, proj_sz, "%s", slash + 1);
    else
        snprintf(proj_out, proj_sz, "-");

    char pkgjson[BUF];
    snprintf(pkgjson, sizeof(pkgjson), "%s/package.json", cwd);
    if (file_exists(pkgjson)) {
        char content[65536] = {0};
        read_file(pkgjson, content, sizeof(content));
        if (str_contains(content, "\"next\""))          { snprintf(fw_out, fw_sz, "Next.js");  return; }
        if (str_contains(content, "\"nuxt\""))          { snprintf(fw_out, fw_sz, "Nuxt.js");  return; }
        if (str_contains(content, "\"vite\""))          { snprintf(fw_out, fw_sz, "Vite");     return; }
        if (str_contains(content, "\"@angular/core\"")) { snprintf(fw_out, fw_sz, "Angular");  return; }
        if (str_contains(content, "\"react\""))         { snprintf(fw_out, fw_sz, "React");    return; }
        if (str_contains(content, "\"svelte\""))        { snprintf(fw_out, fw_sz, "Svelte");   return; }
        if (str_contains(content, "\"@remix-run\""))    { snprintf(fw_out, fw_sz, "Remix");    return; }
        if (str_contains(content, "\"astro\""))         { snprintf(fw_out, fw_sz, "Astro");    return; }
        if (str_contains(content, "\"express\""))       { snprintf(fw_out, fw_sz, "Express");  return; }
        if (str_contains(content, "\"fastify\""))       { snprintf(fw_out, fw_sz, "Fastify");  return; }
        if (str_contains(content, "\"nest\""))          { snprintf(fw_out, fw_sz, "NestJS");   return; }
        snprintf(fw_out, fw_sz, "Node.js");
        return;
    }

    char reqtxt[BUF];
    snprintf(reqtxt, sizeof(reqtxt), "%s/requirements.txt", cwd);
    if (file_exists(reqtxt)) {
        char content[32768] = {0};
        read_file(reqtxt, content, sizeof(content));
        if (str_contains(content, "django"))  { snprintf(fw_out, fw_sz, "Django");  return; }
        if (str_contains(content, "fastapi")) { snprintf(fw_out, fw_sz, "FastAPI"); return; }
        if (str_contains(content, "flask"))   { snprintf(fw_out, fw_sz, "Flask");   return; }
        snprintf(fw_out, fw_sz, "Python");
        return;
    }
    snprintf(reqtxt, sizeof(reqtxt), "%s/pyproject.toml", cwd);
    if (file_exists(reqtxt)) { snprintf(fw_out, fw_sz, "Python"); return; }

    char gemfile[BUF];
    snprintf(gemfile, sizeof(gemfile), "%s/Gemfile", cwd);
    if (file_exists(gemfile)) {
        char content[32768] = {0};
        read_file(gemfile, content, sizeof(content));
        if (str_contains(content, "rails")) { snprintf(fw_out, fw_sz, "Rails"); return; }
        snprintf(fw_out, fw_sz, "Ruby");
        return;
    }

    char gomod[BUF];
    snprintf(gomod, sizeof(gomod), "%s/go.mod", cwd);
    if (file_exists(gomod)) { snprintf(fw_out, fw_sz, "Go"); return; }

    char cargotoml[BUF];
    snprintf(cargotoml, sizeof(cargotoml), "%s/Cargo.toml", cwd);
    if (file_exists(cargotoml)) { snprintf(fw_out, fw_sz, "Rust"); return; }

    char gradlef[BUF], pom[BUF];
    snprintf(gradlef, sizeof(gradlef), "%s/build.gradle", cwd);
    snprintf(pom,     sizeof(pom),     "%s/pom.xml",      cwd);
    if (file_exists(gradlef)) { snprintf(fw_out, fw_sz, "Gradle/JVM"); return; }
    if (file_exists(pom))     { snprintf(fw_out, fw_sz, "Maven/JVM");  return; }

    if (str_contains(cmdline, "python")) { snprintf(fw_out, fw_sz, "Python"); return; }
    if (str_contains(cmdline, "ruby"))   { snprintf(fw_out, fw_sz, "Ruby");   return; }
    if (str_contains(cmdline, "java"))   { snprintf(fw_out, fw_sz, "Java");   return; }
    if (str_contains(cmdline, "go"))     { snprintf(fw_out, fw_sz, "Go");     return; }
    if (str_contains(cmdline, "rust"))   { snprintf(fw_out, fw_sz, "Rust");   return; }

    snprintf(fw_out, fw_sz, "-");
}

static void docker_image_name(const char *image, char *out, size_t sz) {
    if (str_contains(image, "postgres"))   { snprintf(out, sz, "PostgreSQL");    return; }
    if (str_contains(image, "redis"))      { snprintf(out, sz, "Redis");         return; }
    if (str_contains(image, "mongo"))      { snprintf(out, sz, "MongoDB");       return; }
    if (str_contains(image, "mysql"))      { snprintf(out, sz, "MySQL");         return; }
    if (str_contains(image, "mariadb"))    { snprintf(out, sz, "MariaDB");       return; }
    if (str_contains(image, "nginx"))      { snprintf(out, sz, "nginx");         return; }
    if (str_contains(image, "localstack")) { snprintf(out, sz, "LocalStack");    return; }
    if (str_contains(image, "rabbitmq"))   { snprintf(out, sz, "RabbitMQ");      return; }
    if (str_contains(image, "kafka"))      { snprintf(out, sz, "Kafka");         return; }
    if (str_contains(image, "elastic"))    { snprintf(out, sz, "Elasticsearch"); return; }
    snprintf(out, sz, "Docker");
}

/* ════════════════════════════════════════════════════════════════
   PROCESS INFO
   ════════════════════════════════════════════════════════════════ */

/*
 * fill_proc_info — populates process details for a given PID.
 *
 * cwd_hint (macOS only): if non-NULL and non-empty, use this as the CWD
 * instead of calling lsof. This allows scan_procs to batch all CWD
 * lookups into a single lsof call via batch_resolve_cwds().
 * Pass NULL from scan_ports (which handles only a few ports at a time).
 */
static void fill_proc_info(pid_t pid, char *cmdline, size_t cmd_sz,
                            char *cwd_buf, size_t cwd_sz,
                            long *uptime_s, long *mem_kb, float *cpu_pct,
                            char *status, size_t st_sz,
                            const char *cwd_hint) {
    cmdline[0] = '\0'; cwd_buf[0] = '\0';
    *uptime_s = -1; *mem_kb = 0; *cpu_pct = 0.0f;
    snprintf(status, st_sz, "healthy");

#ifdef PLATFORM_LINUX
    /* cwd_hint unused on Linux — we read /proc directly */
    (void)cwd_hint;

    char path[BUF];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(cmdline, 1, cmd_sz - 1, f);
        cmdline[n] = '\0';
        for (size_t i = 0; i < n; i++)
            if (cmdline[i] == '\0') cmdline[i] = ' ';
        str_trim(cmdline);
        fclose(f);
    }

    snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
    ssize_t r = readlink(path, cwd_buf, cwd_sz - 1);
    if (r > 0) cwd_buf[r] = '\0';

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (f) {
        char statbuf[4096];
        if (fgets(statbuf, sizeof(statbuf), f)) {
            char state = '\0';
            long long starttime = 0;
            char *p = strrchr(statbuf, ')');
            if (p) {
                p++;
                int field = 3;
                char *tok = strtok(p, " ");
                while (tok) {
                    if (field == 3) state = tok[0];
                    if (field == 22) {
                        starttime = atoll(tok);
                        long hz = sysconf(_SC_CLK_TCK);
                        if (hz <= 0) hz = 100;
                        FILE *uf = fopen("/proc/uptime", "r");
                        double sys_up = 0;
                        if (uf) { fscanf(uf, "%lf", &sys_up); fclose(uf); }
                        *uptime_s = (long)(sys_up - (double)starttime / hz);
                        if (*uptime_s < 0) *uptime_s = 0;
                    }
                    tok = strtok(NULL, " ");
                    field++;
                }
            }
            if (state == 'Z') snprintf(status, st_sz, "zombie");
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                sscanf(line + 6, "%ld", mem_kb);
                break;
            }
        }
        fclose(f);
    }

    {
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f1 = fopen(path, "r");
        if (f1) {
            char buf1[4096], buf2[4096];
            fgets(buf1, sizeof(buf1), f1); fclose(f1);
            usleep(100000);
            FILE *f2 = fopen(path, "r");
            if (f2) {
                fgets(buf2, sizeof(buf2), f2); fclose(f2);
                long u1=0,s1=0,u2=0,s2=0;
                char *p1 = strrchr(buf1, ')'), *p2 = strrchr(buf2, ')');
                if (p1 && p2) {
                    int fi=3; char *t;
                    t = strtok(p1+1," "); while(t){ if(fi==14)u1=atol(t); if(fi==15){s1=atol(t);break;} t=strtok(NULL," ");fi++; }
                    fi=3; t = strtok(p2+1," "); while(t){ if(fi==14)u2=atol(t); if(fi==15){s2=atol(t);break;} t=strtok(NULL," ");fi++; }
                    long hz = sysconf(_SC_CLK_TCK); if(hz<=0) hz=100;
                    *cpu_pct = (float)((u2+s2)-(u1+s1)) / (hz * 0.1f) * 100.0f;
                    if(*cpu_pct < 0) *cpu_pct = 0;
                }
            }
        }
    }

#else /* PLATFORM_MACOS */
    char cmd[BUF];
    snprintf(cmd, sizeof(cmd), "ps -p %d -o command= 2>/dev/null", pid);
    char *out = run_cmd(cmd);
    if (out) {
        snprintf(cmdline, cmd_sz, "%s", out);
        str_trim(cmdline);
        free(out);
    }

    /* CWD: use pre-resolved hint if available, else fall back to lsof */
    if (cwd_hint && cwd_hint[0]) {
        snprintf(cwd_buf, cwd_sz, "%s", cwd_hint);
    } else {
        snprintf(cmd, sizeof(cmd),
            "lsof -p %d -d cwd -Fn 2>/dev/null | grep '^n' | head -1", pid);
        out = run_cmd(cmd);
        if (out) {
            char *nl = strchr(out, '\n');
            if (nl) *nl = '\0';
            if (out[0] == 'n') snprintf(cwd_buf, cwd_sz, "%s", out + 1);
            free(out);
        }
    }

    snprintf(cmd, sizeof(cmd), "ps -p %d -o etimes=,rss=,%%cpu= 2>/dev/null", pid);
    out = run_cmd(cmd);
    if (out) {
        str_trim(out);
        long et=0, rss=0; float cpu=0;
        sscanf(out, "%ld %ld %f", &et, &rss, &cpu);
        *uptime_s = et;
        *mem_kb   = rss;
        *cpu_pct  = cpu;
        free(out);
    }

    snprintf(cmd, sizeof(cmd), "ps -p %d -o stat= 2>/dev/null", pid);
    out = run_cmd(cmd);
    if (out) {
        str_trim(out);
        if (out[0] == 'Z') snprintf(status, st_sz, "zombie");
        free(out);
    }
#endif
}

/* ════════════════════════════════════════════════════════════════
   DOCKER QUERY
   ════════════════════════════════════════════════════════════════ */

typedef struct { int host_port; char name[SMBUF]; char image[SMBUF]; } DockerPort;
static DockerPort g_docker[256];
static int        g_ndocker = 0;

static void load_docker_ports(void) {
    g_ndocker = 0;
    char *out = run_cmd("docker ps --format '{{.Ports}}\t{{.Names}}\t{{.Image}}' 2>/dev/null");
    if (!out) return;
    char *line = strtok(out, "\n");
    while (line && g_ndocker < 256) {
        char ports_s[BUF]={0}, name[SMBUF]={0}, image[SMBUF]={0};
        char *tab1 = strchr(line, '\t');
        if (!tab1) { line=strtok(NULL,"\n"); continue; }
        *tab1 = '\0';
        snprintf(ports_s, sizeof(ports_s), "%s", line);
        char *tab2 = strchr(tab1+1, '\t');
        if (tab2) {
            *tab2 = '\0';
            snprintf(name,  sizeof(name),  "%s", tab1+1);
            snprintf(image, sizeof(image), "%s", tab2+1);
        } else {
            snprintf(name, sizeof(name), "%s", tab1+1);
        }
        str_trim(name); str_trim(image);
        char *p = ports_s;
        while (*p) {
            char *colon = strchr(p, ':');
            if (!colon) break;
            int hp = atoi(colon+1);
            if (hp > 0 && g_ndocker < 256) {
                g_docker[g_ndocker].host_port = hp;
                snprintf(g_docker[g_ndocker].name,  sizeof(g_docker[g_ndocker].name),  "%s", name);
                snprintf(g_docker[g_ndocker].image, sizeof(g_docker[g_ndocker].image), "%s", image);
                g_ndocker++;
            }
            char *arr = strstr(colon, "->");
            p = arr ? arr + 2 : p + strlen(p);
            while (*p == ',' || *p == ' ') p++;
        }
        line = strtok(NULL, "\n");
    }
    free(out);
}

static DockerPort *docker_for_port(int port) {
    for (int i = 0; i < g_ndocker; i++)
        if (g_docker[i].host_port == port) return &g_docker[i];
    return NULL;
}

/* ════════════════════════════════════════════════════════════════
   SYSTEM-PROCESS FILTER
   ════════════════════════════════════════════════════════════════ */

static const char *g_skip_procs[] = {
    "spotify","slack","zoom","discord","chrome","firefox","safari",
    "code","cursor","electron","signal","telegram","whatsapp",
    "dropbox","1password","raycast","alfred","keybase",
    "com.apple","launchd","systemd","avahi","dbus",
    "sshd","cupsd","bluetoothd","wifid","mDNSResponder",
    "ntpd","chronyd","polkitd","NetworkManager","nm-",
    NULL
};

static int is_system_proc(const char *proc, const char *cmdline) {
    for (int i = 0; g_skip_procs[i]; i++)
        if (str_contains(proc, g_skip_procs[i]) ||
            str_contains(cmdline, g_skip_procs[i])) return 1;
    return 0;
}

static const char *g_dev_procs[] = {
    "node","python","python3","ruby","java","go","cargo","rust",
    "uvicorn","gunicorn","puma","unicorn","rails","django",
    "php","lua","bun","deno","tsx","ts-node","nodemon","webpack",
    "vite","next","nuxt","astro","remix","gradle","mvn",
    "docker","docker-proxy","containerd",
    NULL
};

static int is_dev_proc(const char *proc, const char *cmdline) {
    for (int i = 0; g_dev_procs[i]; i++)
        if (str_contains(proc, g_dev_procs[i]) ||
            str_contains(cmdline, g_dev_procs[i])) return 1;
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   PORT SCAN
   ════════════════════════════════════════════════════════════════ */

static void scan_ports(void) {
    g_nports = 0;
    load_docker_ports();

    char *out = run_cmd("lsof -iTCP -sTCP:LISTEN -nP 2>/dev/null");
    if (!out) return;

    char *line = strtok(out, "\n");
    while (line && g_nports < MAX_PORTS) {
        if (strncmp(line, "COMMAND", 7) == 0) { line=strtok(NULL,"\n"); continue; }
        char proc[SMBUF]={0}, pidstr[SMBUF]={0}, name[SMBUF]={0};
        int n = sscanf(line, "%255s %255s %*s %*s %*s %*s %*s %*s %255s",
                       proc, pidstr, name);
        if (n < 3) { line=strtok(NULL,"\n"); continue; }

        pid_t pid = (pid_t)atoi(pidstr);
        int port = 0;
        char *colon = strrchr(name, ':');
        if (colon) port = atoi(colon + 1);
        if (port <= 0) { line=strtok(NULL,"\n"); continue; }

        PortEntry *pe = &g_ports[g_nports];
        memset(pe, 0, sizeof(*pe));
        pe->port = port;
        pe->pid  = pid;
        snprintf(pe->proto,   sizeof(pe->proto),   "tcp");
        snprintf(pe->process, sizeof(pe->process), "%s", proc);

        if (str_contains(proc, "docker") || str_contains(proc, "com.docker")) {
            DockerPort *dp = docker_for_port(port);
            if (dp) {
                pe->is_docker = 1;
                snprintf(pe->container, sizeof(pe->container), "%s", dp->name);
                docker_image_name(dp->image, pe->framework, sizeof(pe->framework));
                snprintf(pe->project, sizeof(pe->project), "%s", dp->name);
                snprintf(pe->status,  sizeof(pe->status),  "healthy");
                pe->uptime_s = -1;
                g_nports++;
                line = strtok(NULL,"\n");
                continue;
            }
        }

        /* NULL hint: scan_ports handles few ports, per-pid lsof is fine */
        fill_proc_info(pid, pe->cmdline, sizeof(pe->cmdline),
                       pe->cwd, sizeof(pe->cwd),
                       &pe->uptime_s, &pe->mem_kb, &pe->cpu_pct,
                       pe->status, sizeof(pe->status),
                       NULL);

        if (pe->cwd[0])
            detect_framework(pe->cwd, pe->cmdline,
                             pe->framework, sizeof(pe->framework),
                             pe->project,   sizeof(pe->project));
        else {
            snprintf(pe->project,   sizeof(pe->project),   "-");
            snprintf(pe->framework, sizeof(pe->framework), "-");
        }

        if (!strcmp(pe->status, "healthy") && pe->cwd[0]) {
            struct stat st;
            if (stat(pe->cwd, &st) != 0)
                snprintf(pe->status, sizeof(pe->status), "orphaned");
        }

        if (!g_show_all && is_system_proc(pe->process, pe->cmdline)) {
            line = strtok(NULL, "\n");
            continue;
        }

        g_nports++;
        line = strtok(NULL, "\n");
    }
    free(out);
}

/* ════════════════════════════════════════════════════════════════
   BATCH CWD RESOLUTION  (macOS only)
   ════════════════════════════════════════════════════════════════ */

#ifdef PLATFORM_MACOS
/*
 * Resolve CWDs for multiple PIDs with a single lsof call instead of
 * one lsof call per PID. This is the fix for `ports ps --all` being slow.
 *
 * pids[count]  — input PID array
 * cwds[count]  — output: cwds[i] receives the CWD for pids[i], or "" if not found
 * Each cwd entry is PATH_MAX+1 bytes.
 */
static void batch_resolve_cwds(pid_t *pids, char (*cwds)[PATH_MAX + 1], int count) {
    if (count <= 0) return;

    /* Build comma-separated PID list: max 512 PIDs × 7 digits + comma = ~4096 bytes */
    char pidlist[MAX_PROCS * 8];
    int off = 0;
    for (int i = 0; i < count && off < (int)sizeof(pidlist) - 8; i++)
        off += snprintf(pidlist + off, sizeof(pidlist) - (size_t)off,
                        i == 0 ? "%d" : ",%d", pids[i]);

    char cmd[sizeof(pidlist) + 64];
    snprintf(cmd, sizeof(cmd), "lsof -p %s -d cwd -Fn 2>/dev/null", pidlist);
    char *out = run_cmd(cmd);
    if (!out) return;

    /*
     * lsof -Fn output format:
     *   p<pid>
     *   f<fd>
     *   n<path>
     * We only care about 'p' and 'n' lines.
     */
    pid_t cur = -1;
    char *line = strtok(out, "\n");
    while (line) {
        if (line[0] == 'p') {
            cur = (pid_t)atoi(line + 1);
        } else if (line[0] == 'n' && cur > 0) {
            for (int i = 0; i < count; i++) {
                if (pids[i] == cur) {
                    snprintf(cwds[i], PATH_MAX + 1, "%s", line + 1);
                    break;
                }
            }
            cur = -1; /* reset: each pid has exactly one cwd */
        }
        line = strtok(NULL, "\n");
    }
    free(out);
}
#endif /* PLATFORM_MACOS */

/* ════════════════════════════════════════════════════════════════
   PROCESS SCAN
   ════════════════════════════════════════════════════════════════ */

static void scan_procs(void) {
    g_nprocs = 0;

#ifdef PLATFORM_LINUX
    const char *cmd =
        "ps aux --no-header 2>/dev/null | awk '{print $2,$3,$4,$11}'";
#else
    const char *cmd =
        "ps aux 2>/dev/null | tail -n +2 | awk '{print $2,$3,$4,$11}'";
#endif

    char *out = run_cmd(cmd);
    if (!out) return;

    int   docker_count    = 0;
    pid_t docker_root_pid = 0;
    long  docker_total_mem = 0;
    float docker_total_cpu = 0;

    /* ── Pass 1: filter and collect PIDs + basic fields ── */
    typedef struct { pid_t pid; char proc[SMBUF]; float cpu; } RawProc;
    static RawProc raw[MAX_PROCS];
    int nraw = 0;

    char *line = strtok(out, "\n");
    while (line && nraw < MAX_PROCS) {
        char pidstr[SMBUF]={0}, cpustr[SMBUF]={0}, memstr[SMBUF]={0}, proc[SMBUF]={0};
        sscanf(line, "%255s %255s %255s %255s", pidstr, cpustr, memstr, proc);
        pid_t pid = (pid_t)atoi(pidstr);
        char *bn = strrchr(proc, '/');
        if (bn) memmove(proc, bn+1, strlen(bn));

        if (!g_show_all && !is_dev_proc(proc, ""))   { line=strtok(NULL,"\n"); continue; }
        if (!g_show_all && is_system_proc(proc, "")) { line=strtok(NULL,"\n"); continue; }

        if (str_contains(proc, "docker") || str_contains(proc, "containerd") ||
            str_contains(proc, "com.docker")) {
            docker_count++;
            docker_total_cpu += (float)atof(cpustr);
            docker_total_mem += (long)(atof(memstr) * 1024);
            if (docker_root_pid == 0 || pid < docker_root_pid)
                docker_root_pid = pid;
            line = strtok(NULL, "\n");
            continue;
        }

        raw[nraw].pid = pid;
        raw[nraw].cpu = (float)atof(cpustr);
        snprintf(raw[nraw].proc, SMBUF, "%s", proc);
        nraw++;
        line = strtok(NULL, "\n");
    }
    free(out);

#ifdef PLATFORM_MACOS
    /* ── Batch resolve all CWDs in one lsof call ── */
    static pid_t batch_pids[MAX_PROCS];
    static char  batch_cwds[MAX_PROCS][PATH_MAX + 1];
    memset(batch_cwds, 0, sizeof(batch_cwds));
    for (int i = 0; i < nraw; i++) batch_pids[i] = raw[i].pid;
    batch_resolve_cwds(batch_pids, batch_cwds, nraw);
#endif

    /* ── Pass 2: fill full proc info using pre-resolved CWDs ── */
    for (int i = 0; i < nraw && g_nprocs < MAX_PROCS; i++) {
        ProcEntry *pe = &g_procs[g_nprocs];
        memset(pe, 0, sizeof(*pe));
        pe->pid = raw[i].pid;
        snprintf(pe->process, sizeof(pe->process), "%s", raw[i].proc);
        pe->cpu_pct = raw[i].cpu;

#ifdef PLATFORM_MACOS
        const char *hint = batch_cwds[i];
#else
        const char *hint = NULL;
#endif
        fill_proc_info(raw[i].pid, pe->cmdline, sizeof(pe->cmdline),
                       pe->cwd, sizeof(pe->cwd),
                       &pe->uptime_s, &pe->mem_kb, &pe->cpu_pct,
                       pe->status, sizeof(pe->status),
                       hint);

        if (pe->cwd[0])
            detect_framework(pe->cwd, pe->cmdline,
                             pe->framework, sizeof(pe->framework),
                             pe->project,   sizeof(pe->project));
        else {
            snprintf(pe->project,   sizeof(pe->project),   "-");
            snprintf(pe->framework, sizeof(pe->framework), "-");
        }

        /* shorten cmdline for WHAT column */
        char *sp = strchr(pe->cmdline, ' ');
        if (sp) {
            char *arg = sp + 1;
            if (*arg != '-') {
                char *last = strrchr(arg, '/');
                if (last) memmove(pe->cmdline, last+1, strlen(last));
                else      memmove(pe->cmdline, arg, strlen(arg)+1);
            }
        }
        str_trim(pe->cmdline);
        g_nprocs++;
    }

    if (docker_count > 0 && g_nprocs + 1 < MAX_PROCS) {
        memmove(&g_procs[1], &g_procs[0], sizeof(ProcEntry) * (size_t)g_nprocs);
        g_nprocs++;
        ProcEntry *dp = &g_procs[0];
        memset(dp, 0, sizeof(*dp));
        dp->pid = docker_root_pid;
        snprintf(dp->process,   sizeof(dp->process),   "Docker");
        snprintf(dp->framework, sizeof(dp->framework), "Docker");
        snprintf(dp->project,   sizeof(dp->project),   "-");
        snprintf(dp->status,    sizeof(dp->status),    "healthy");
        dp->cpu_pct = docker_total_cpu;
        dp->mem_kb  = docker_total_mem;
        dp->is_docker_group = 1;
        dp->docker_count = docker_count;
        dp->uptime_s = -1;
    }
}

/* ════════════════════════════════════════════════════════════════
   TABLE RENDERING
   ════════════════════════════════════════════════════════════════ */

static void box_rule(const char *left, const char *mid, const char *right,
                     const int *widths, int ncols) {
    printf("%s", left);
    for (int c = 0; c < ncols; c++) {
        for (int w = 0; w < widths[c] + 2; w++) printf("%s", BOX_H);
        printf("%s", c < ncols-1 ? mid : right);
    }
    printf("\n");
}

static const char *status_render(const char *s) {
    static char buf[128];
    if (!strcmp(s, "healthy"))
        snprintf(buf, sizeof(buf), DOT_GREEN  " " COL_BGREEN  "healthy"  COL_RESET);
    else if (!strcmp(s, "orphaned"))
        snprintf(buf, sizeof(buf), DOT_YELLOW " " COL_BYELLOW "orphaned" COL_RESET);
    else if (!strcmp(s, "zombie"))
        snprintf(buf, sizeof(buf), DOT_RED    " " COL_BRED    "zombie"   COL_RESET);
    else
        snprintf(buf, sizeof(buf), "%s", s);
    return buf;
}

static void print_cell(const char *s, int width, int last) {
    int dw = display_width(s);
    printf(" %s", s);
    for (int i = dw; i < width; i++) printf(" ");
    printf(" %s", last ? BOX_V "\n" : BOX_V);
}

/* ── ports table ─────────────────────────────────────────────── */

static void print_ports_table(void) {
#define BANNER_INNER 30
    printf("\n");
    // Top Border
    printf(" " BOX_TL);
    for (int i = 0; i < BANNER_INNER; i++) printf(BOX_H);
    printf(BOX_TR "\n");
    // Line 1: "Port Whisperer" (14 visible chars)
    // Formula: BANNER_INNER - 2 (leading spaces) - 14 (text length)
    printf(" " BOX_V "  " COL_CYAN COL_BOLD "Port Whisperer" COL_RESET "%*s" BOX_V "\n",
           (BANNER_INNER - 2 - 14), "");
    // Line 2: "listening to your ports..." (26 visible chars)
    // Formula: BANNER_INNER - 2 (leading spaces) - 26 (text length)
    printf(" " BOX_V "  " COL_DIM "listening to your ports..." COL_RESET "%*s" BOX_V "\n",
           (BANNER_INNER - 2 - 26), "");
    // Bottom Border
    printf(" " BOX_BL);
    for (int i = 0; i < BANNER_INNER; i++) printf(BOX_H);
    printf(BOX_BR "\n\n");
#undef BANNER_INNER

    if (g_nports == 0) {
        printf(COL_DIM "  No ports found.\n" COL_RESET);
        return;
    }

    const char *headers[] = { "PORT", "PROCESS", "PID", "PROJECT",
                               "FRAMEWORK", "UPTIME", "STATUS" };
    int ncols = 7;
    int widths[7] = { 6, 8, 6, 12, 10, 8, 10 };

    for (int i = 0; i < g_nports; i++) {
        PortEntry *pe = &g_ports[i];
        char portstr[16]; snprintf(portstr, sizeof(portstr), ":%d", pe->port);
        char pidstr[16];  snprintf(pidstr,  sizeof(pidstr),  "%d", pe->pid);
        char upstr[32];   fmt_uptime(pe->uptime_s, upstr, sizeof(upstr));
        int lens[7] = {
            display_width(portstr),
            display_width(pe->process),
            display_width(pidstr),
            display_width(pe->project),
            display_width(pe->framework),
            display_width(upstr),
            display_width(pe->status) + 2
        };
        for (int c = 0; c < ncols; c++)
            if (lens[c] > widths[c]) widths[c] = lens[c];
    }

    box_rule(BOX_TL, BOX_TM, BOX_TR, widths, ncols);
    printf(BOX_V);
    for (int c = 0; c < ncols; c++) {
        printf(" " COL_BOLD "%s" COL_RESET, headers[c]);
        for (int p = (int)strlen(headers[c]); p < widths[c]; p++) printf(" ");
        printf(" %s", BOX_V);
    }
    printf("\n");
    box_rule(BOX_ML, BOX_MM, BOX_MR, widths, ncols);

    for (int i = 0; i < g_nports; i++) {
        PortEntry *pe = &g_ports[i];
        char portstr[16]; snprintf(portstr, sizeof(portstr), ":%d", pe->port);
        char pidstr[16];  snprintf(pidstr,  sizeof(pidstr),  "%d", pe->pid);
        char upstr[32];   fmt_uptime(pe->uptime_s, upstr, sizeof(upstr));

        char colored_port[64];
        snprintf(colored_port, sizeof(colored_port), COL_CYAN "%s" COL_RESET, portstr);

        printf(BOX_V);
        print_cell(colored_port,   widths[0], 0);
        print_cell(pe->process,    widths[1], 0);
        print_cell(pidstr,         widths[2], 0);
        print_cell(pe->project,    widths[3], 0);
        print_cell(pe->framework,  widths[4], 0);
        print_cell(upstr,          widths[5], 0);
        const char *sr = status_render(pe->status);
        int vl = display_width(sr);
        printf(" %s", sr);
        for (int p = vl; p < widths[6]; p++) printf(" ");
        printf(" " BOX_V "\n");

        if (i < g_nports - 1)
            box_rule(BOX_ML, BOX_MM, BOX_MR, widths, ncols);
    }
    box_rule(BOX_BL, BOX_BM, BOX_BR, widths, ncols);

    printf("\n  " COL_BOLD "%d" COL_RESET " port%s active",
           g_nports, g_nports == 1 ? "" : "s");
    printf("  .  Run " COL_CYAN "ports <number>" COL_RESET " for details");
    printf("  .  " COL_DIM "--all" COL_RESET " to show everything\n\n");
}

/* ── process table ───────────────────────────────────────────── */

static void print_procs_table(void) {
    printf("\n");
    printf(" " COL_CYAN COL_BOLD "Dev Processes" COL_RESET "\n\n");

    if (g_nprocs == 0) {
        printf(COL_DIM "  No dev processes found.\n" COL_RESET);
        return;
    }

    const char *headers[] = { "PID", "PROCESS", "CPU%", "MEM", "PROJECT",
                               "FRAMEWORK", "UPTIME", "WHAT" };
    int ncols = 8;
    int widths[8] = { 6, 8, 5, 7, 10, 10, 8, 24 };

    for (int i = 0; i < g_nprocs; i++) {
        ProcEntry *pe = &g_procs[i];
        char pidstr[16], cpustr[12], memstr[16], upstr[32];
        snprintf(pidstr, sizeof(pidstr), "%d",   pe->pid);
        snprintf(cpustr, sizeof(cpustr), "%.1f", pe->cpu_pct);
        fmt_mem(pe->mem_kb, memstr, sizeof(memstr));
        fmt_uptime(pe->uptime_s, upstr, sizeof(upstr));
        int lens[8] = {
            display_width(pidstr),      display_width(pe->process),
            display_width(cpustr),      display_width(memstr),
            display_width(pe->project), display_width(pe->framework),
            display_width(upstr),       display_width(pe->cmdline)
        };
        for (int c = 0; c < ncols; c++)
            if (lens[c] > widths[c]) widths[c] = lens[c];
    }
    if (widths[7] > 40) widths[7] = 40;

    box_rule(BOX_TL, BOX_TM, BOX_TR, widths, ncols);
    printf(BOX_V);
    for (int c = 0; c < ncols; c++) {
        printf(" " COL_BOLD "%s" COL_RESET, headers[c]);
        for (int p = (int)strlen(headers[c]); p < widths[c]; p++) printf(" ");
        printf(" %s", BOX_V);
    }
    printf("\n");
    box_rule(BOX_ML, BOX_MM, BOX_MR, widths, ncols);

    for (int i = 0; i < g_nprocs; i++) {
        ProcEntry *pe = &g_procs[i];
        char pidstr[16], cpustr[12], memstr[16], upstr[32], what[64];
        snprintf(pidstr, sizeof(pidstr), "%d",   pe->pid);
        snprintf(cpustr, sizeof(cpustr), "%.1f", pe->cpu_pct);
        fmt_mem(pe->mem_kb, memstr, sizeof(memstr));
        fmt_uptime(pe->uptime_s, upstr, sizeof(upstr));
        if (pe->is_docker_group)
            snprintf(what, sizeof(what), "%d processes", pe->docker_count);
        else
            snprintf(what, sizeof(what), "%.39s", pe->cmdline);

        printf(BOX_V);
        print_cell(pidstr,        widths[0], 0);
        print_cell(pe->process,   widths[1], 0);
        print_cell(cpustr,        widths[2], 0);
        print_cell(memstr,        widths[3], 0);
        print_cell(pe->project,   widths[4], 0);
        print_cell(pe->framework, widths[5], 0);
        print_cell(upstr,         widths[6], 0);
        print_cell(what,          widths[7], 1);

        if (i < g_nprocs - 1)
            box_rule(BOX_ML, BOX_MM, BOX_MR, widths, ncols);
    }
    box_rule(BOX_BL, BOX_BM, BOX_BR, widths, ncols);

    printf("\n  " COL_BOLD "%d" COL_RESET " process%s",
           g_nprocs, g_nprocs == 1 ? "" : "es");
    printf("  .  " COL_DIM "--all" COL_RESET " to show everything\n\n");
}

/* ════════════════════════════════════════════════════════════════
   INSPECT SINGLE PORT
   ════════════════════════════════════════════════════════════════ */

static void inspect_port(int target) {
    scan_ports();
    PortEntry *pe = NULL;
    for (int i = 0; i < g_nports; i++)
        if (g_ports[i].port == target) { pe = &g_ports[i]; break; }

    if (!pe) {
        g_show_all = 1;
        scan_ports();
        for (int i = 0; i < g_nports; i++)
            if (g_ports[i].port == target) { pe = &g_ports[i]; break; }
    }

    if (!pe) {
        printf(COL_YELLOW "\n  Port %d is not in use.\n\n" COL_RESET, target);
        return;
    }

    char upstr[32], memstr[32];
    fmt_uptime(pe->uptime_s, upstr, sizeof(upstr));
    fmt_mem(pe->mem_kb, memstr, sizeof(memstr));

    /* Git branch */
    char branch[SMBUF] = "-";
    if (pe->cwd[0]) {
        char safe_cwd[PATH_MAX + 1];
        char cmd[PATH_MAX + 128];
        snprintf(safe_cwd, sizeof(safe_cwd), "%s", pe->cwd);
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null", safe_cwd);
        char *gb = run_cmd(cmd);
        if (gb) { str_trim(gb); snprintf(branch, sizeof(branch), "%s", gb); free(gb); }
    }

    printf("\n");
    printf(" " COL_CYAN COL_BOLD "Port :%d" COL_RESET "\n\n", target);
    printf("  Process   : " COL_BOLD "%s" COL_RESET " (PID %d)\n", pe->process, pe->pid);
    printf("  Project   : " COL_BOLD "%s" COL_RESET "\n", pe->project);
    printf("  Framework : " COL_BOLD "%s" COL_RESET "\n", pe->framework);
    printf("  CWD       : " COL_DIM "%s" COL_RESET "\n", pe->cwd[0] ? pe->cwd : "-");
    printf("  Git branch: " COL_BOLD "%s" COL_RESET "\n", branch);
    printf("  Uptime    : " COL_BOLD "%s" COL_RESET "\n", upstr);
    printf("  Memory    : " COL_BOLD "%s" COL_RESET "\n", memstr);
    printf("  CPU       : " COL_BOLD "%.1f%%" COL_RESET "\n", pe->cpu_pct);
    printf("  Status    : %s\n", status_render(pe->status));
    if (pe->cmdline[0])
        printf("  Cmdline   : " COL_DIM "%.80s" COL_RESET "\n", pe->cmdline);
    printf("\n");

    printf("  Kill process? [y/N] ");
    fflush(stdout);
    char ans[8] = {0};
    if (fgets(ans, sizeof(ans), stdin)) {
        if (ans[0] == 'y' || ans[0] == 'Y') {
            if (kill(pe->pid, SIGTERM) == 0)
                printf(COL_GREEN "  Sent SIGTERM to PID %d.\n" COL_RESET, pe->pid);
            else
                printf(COL_RED "  Failed: %s\n" COL_RESET, strerror(errno));
        } else {
            printf("  Aborted.\n");
        }
    }
    printf("\n");
}

/* ════════════════════════════════════════════════════════════════
   WATCH MODE
   ════════════════════════════════════════════════════════════════ */

static void watch_ports(void) {
    printf(COL_CYAN "\n  Watching for port changes... (Ctrl-C to stop)\n\n" COL_RESET);
    fflush(stdout);

    int prev_ports[MAX_PORTS];
    int prev_count = 0;

    while (1) {
        scan_ports();
        int cur_ports[MAX_PORTS];
        for (int i = 0; i < g_nports; i++) cur_ports[i] = g_ports[i].port;

        for (int i = 0; i < g_nports; i++) {
            int found = 0;
            for (int j = 0; j < prev_count; j++)
                if (prev_ports[j] == cur_ports[i]) { found = 1; break; }
            if (!found) {
                PortEntry *pe = &g_ports[i];
                char ts[32];
                time_t t = time(NULL);
                struct tm *tm = localtime(&t);
                strftime(ts, sizeof(ts), "%H:%M:%S", tm);
                printf(COL_BGREEN "  [%s] +" COL_RESET " :%d  %s (%s)\n",
                       ts, pe->port, pe->process, pe->framework);
                fflush(stdout);
            }
        }
        for (int i = 0; i < prev_count; i++) {
            int found = 0;
            for (int j = 0; j < g_nports; j++)
                if (cur_ports[j] == prev_ports[i]) { found = 1; break; }
            if (!found) {
                char ts[32];
                time_t t = time(NULL);
                struct tm *tm = localtime(&t);
                strftime(ts, sizeof(ts), "%H:%M:%S", tm);
                printf(COL_BRED "  [%s] -" COL_RESET " :%d  (closed)\n",
                       ts, prev_ports[i]);
                fflush(stdout);
            }
        }

        prev_count = g_nports;
        for (int i = 0; i < g_nports; i++) prev_ports[i] = cur_ports[i];
        sleep(2);
    }
}

/* ════════════════════════════════════════════════════════════════
   CLEAN
   ════════════════════════════════════════════════════════════════ */

static void clean_orphans(void) {
    scan_ports();
    int count = 0;
    for (int i = 0; i < g_nports; i++) {
        PortEntry *pe = &g_ports[i];
        if (strcmp(pe->status, "orphaned") != 0 &&
            strcmp(pe->status, "zombie")   != 0) continue;
        if (!is_dev_proc(pe->process, pe->cmdline)) continue;
        printf(COL_YELLOW "  [clean] " COL_RESET
               "Killing %s (PID %d) on :%d  %s\n",
               pe->process, pe->pid, pe->port, pe->status);
        kill(pe->pid, SIGTERM);
        count++;
    }
    if (count == 0)
        printf(COL_GREEN "  No orphaned dev processes found.\n" COL_RESET);
    else
        printf(COL_GREEN "  Sent SIGTERM to %d process%s.\n" COL_RESET,
               count, count == 1 ? "" : "es");
    printf("\n");
}

/* ════════════════════════════════════════════════════════════════
   HELP
   ════════════════════════════════════════════════════════════════ */

static void print_help(void) {
    printf("\n" COL_BOLD COL_CYAN "port-whisperer" COL_RESET
           " -- bare-metal port inspector\n\n");
    printf("  " COL_BOLD "ports" COL_RESET "                Show dev/docker ports\n");
    printf("  " COL_BOLD "ports --all" COL_RESET "          Show all listening ports\n");
    printf("  " COL_BOLD "ports <number>" COL_RESET "       Inspect a specific port\n");
    printf("  " COL_BOLD "ports ps" COL_RESET "             Show all dev processes\n");
    printf("  " COL_BOLD "ports ps --all" COL_RESET "       Show all processes\n");
    printf("  " COL_BOLD "ports watch" COL_RESET "          Watch for port changes (live)\n");
    printf("  " COL_BOLD "ports clean" COL_RESET "          Kill orphaned dev processes\n");
    printf("  " COL_BOLD "ports --help" COL_RESET "         This help\n");
    printf("\n  Status colours: "
           DOT_GREEN " healthy  "
           DOT_YELLOW " orphaned  "
           DOT_RED " zombie\n\n");
}

/* ════════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--all")) g_show_all = 1;

    if (argc < 2) {
        scan_ports();
        print_ports_table();
        return 0;
    }

    const char *sub = argv[1];

    if (!strcmp(sub, "--help") || !strcmp(sub, "-h")) {
        print_help();
        return 0;
    }

    if (!strcmp(sub, "--all")) {
        scan_ports();
        print_ports_table();
        return 0;
    }

    if (!strcmp(sub, "ps")) {
        scan_procs();
        print_procs_table();
        return 0;
    }

    if (!strcmp(sub, "watch")) {
        watch_ports();
        return 0;
    }

    if (!strcmp(sub, "clean")) {
        clean_orphans();
        return 0;
    }

    char *end;
    long port = strtol(sub, &end, 10);
    if (*end == '\0' && port > 0 && port <= 65535) {
        inspect_port((int)port);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", sub);
    print_help();
    return 1;
}
