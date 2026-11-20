#ifndef SPIRE_JOBS_H
#define SPIRE_JOBS_H

#include "common.h"
#include <sys/types.h>

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct Job {
    int id;
    pid_t pgid;
    char *cmdline;
    JobState state;
    int last_status;
    bool notified;
    struct Job *next;
} Job;

void jobs_init(void);
Job *job_add(pid_t pgid, const char *cmdline);
void job_remove(Job *j);
Job *job_find(int id);
Job *job_find_by_pgid(pid_t pgid);
/* reap any children that have changed state (non-blocking); print
 * "[n]  Done   cmdline" notices for background jobs that finished. */
void jobs_reap(bool print_notices);
void jobs_print(void);
int jobs_count(void);
Job *jobs_head(void);          /* newest first */
/* resolves a job spec: %N, %+, %%, %-, %name, or a bare N; NULL = most recent */
Job *job_from_spec(const char *spec);

#endif
