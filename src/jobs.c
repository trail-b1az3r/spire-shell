#include "jobs.h"
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>

static Job *g_jobs = NULL;
static int g_next_id = 1;

void jobs_init(void) { g_jobs = NULL; g_next_id = 1; }

Job *job_add(pid_t pgid, const char *cmdline) {
    Job *j = xmalloc(sizeof(Job));
    j->id = g_next_id++;
    j->pgid = pgid;
    j->cmdline = xstrdup(cmdline);
    j->state = JOB_RUNNING;
    j->last_status = 0;
    j->notified = false;
    j->next = g_jobs;
    g_jobs = j;
    return j;
}

void job_remove(Job *j) {
    Job **pp = &g_jobs;
    while (*pp) {
        if (*pp == j) {
            *pp = j->next;
            free(j->cmdline);
            free(j);
            return;
        }
        pp = &(*pp)->next;
    }
}

Job *job_find(int id) {
    for (Job *j = g_jobs; j; j = j->next) if (j->id == id) return j;
    return NULL;
}

Job *job_find_by_pgid(pid_t pgid) {
    for (Job *j = g_jobs; j; j = j->next) if (j->pgid == pgid) return j;
    return NULL;
}

int jobs_count(void) {
    int n = 0;
    for (Job *j = g_jobs; j; j = j->next) if (j->state != JOB_DONE) n++;
    return n;
}

void jobs_reap(bool print_notices) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        for (Job *j = g_jobs; j; j = j->next) {
            if (j->pgid == pid) {
                if (WIFSTOPPED(status)) { j->state = JOB_STOPPED; }
                else {
                    j->state = JOB_DONE;
                    j->last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                }
            }
        }
    }
    if (print_notices) {
        Job *j = g_jobs, *prev = NULL;
        while (j) {
            Job *next = j->next;
            if (j->state == JOB_DONE) {
                if (!j->notified) {
                    if (isatty(STDERR_FILENO)) fprintf(stderr, "\x1b[1m[%d]\x1b[0m  %s%-22s\x1b[0m  %s\n", j->id, j->last_status ? "\x1b[31m" : "\x1b[32m", j->last_status ? "Exit" : "Done", j->cmdline);
                    else fprintf(stderr, "[%d]  Done                    %s\n", j->id, j->cmdline);
                }
                if (prev) prev->next = next; else g_jobs = next;
                free(j->cmdline);
                free(j);
                j = next;
                continue;
            }
            prev = j;
            j = next;
        }
    }
}

Job *jobs_head(void) { return g_jobs; }

Job *job_from_spec(const char *spec) {
    if (!spec || !*spec || strcmp(spec, "%+") == 0 || strcmp(spec, "%%") == 0 || strcmp(spec, "%") == 0) return g_jobs;
    if (strcmp(spec, "%-") == 0) return g_jobs ? g_jobs->next : NULL;
    const char *s = spec[0] == '%' ? spec + 1 : spec;
    if (*s >= '0' && *s <= '9') return job_find(atoi(s));
    size_t n = strlen(s);
    for (Job *j = g_jobs; j; j = j->next) if (strncmp(j->cmdline, s, n) == 0) return j;
    return NULL;
}

void jobs_print(void) {
    bool color = isatty(STDOUT_FILENO);
    /* oldest first, like bash */
    int n = 0;
    for (Job *j = g_jobs; j; j = j->next) n++;
    Job **arr = xmalloc(sizeof(Job *) * (size_t)(n ? n : 1));
    int k = n;
    for (Job *j = g_jobs; j; j = j->next) arr[--k] = j;
    for (int i = 0; i < n; i++) {
        Job *j = arr[i];
        const char *state = j->state == JOB_RUNNING ? "Running" : j->state == JOB_STOPPED ? "Stopped" : "Done";
        const char *col = j->state == JOB_RUNNING ? "\x1b[32m" : j->state == JOB_STOPPED ? "\x1b[33m" : "\x1b[90m";
        char mark = (j == g_jobs) ? '+' : (g_jobs && j == g_jobs->next) ? '-' : ' ';
        if (color) printf("\x1b[1m[%d]\x1b[0m%c %s%-8s\x1b[0m  %s\n", j->id, mark, col, state, j->cmdline);
        else printf("[%d]%c %-8s  %s\n", j->id, mark, state, j->cmdline);
    }
    free(arr);
}
