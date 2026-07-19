#ifndef SILEX_JOB_H
#define SILEX_JOB_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <sys/types.h>
#include <signal.h>

/* Job state. A job is a whole pipeline; `pgid` is the process group all its
 * processes belong to, so signals and waits can address the group. */
typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

typedef struct job {
    int          id;        /* job number, as shown by `jobs` and named with %n */
    pid_t        pid;       /* pid of the pipeline's last process (its $! / leader) */
    pid_t        pgid;      /* process group id of the whole pipeline */
    int          status;    /* last waitpid() status */
    job_state_t  state;
    int          notified;  /* 1 once a Done/Stopped transition has been reported */
    char        *command;   /* text of the pipeline, for `jobs` (malloc'd, owned) */
    struct job  *next;
} job_t;

typedef struct {
    job_t *head;
    int    current;   /* job id of the current job (%+ / %%)   */
    int    previous;  /* job id of the previous job (%-)       */
    int    next_id;   /* lowest free job number to assign next */
} job_list_t;

void   job_list_init(job_list_t *jl);
void   job_list_free(job_list_t *jl);

/* Register a new background/stopped job (a whole pipeline). `command` is copied.
 * Assigns a job number and makes it the current job. */
job_t *job_register(job_list_t *jl, pid_t pid, pid_t pgid, const char *command);

/* Legacy helper kept for callers that only track a bare pid (no command text). */
job_t *job_add(job_list_t *jl, pid_t pid);

job_t *job_find_by_id(job_list_t *jl, int id);
job_t *job_find_by_pid(job_list_t *jl, pid_t pid);
job_t *job_current(job_list_t *jl);    /* %+ / %% */
job_t *job_previous(job_list_t *jl);   /* %-      */
void   job_remove(job_list_t *jl, job_t *j);

int    job_wait(job_list_t *jl, pid_t pid);   /* wait for a pid, return exit code */
void   job_reap(job_list_t *jl);              /* non-blocking: update states (WUNTRACED) */
pid_t  job_last_bg(job_list_t *jl);           /* most recently added background pid */

#endif
