#ifndef SILEX_JOB_H
#define SILEX_JOB_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <sys/types.h>
#include <signal.h>

typedef struct job {
    pid_t        pid;
    int          status;    /* from waitpid */
    int          done;
    struct job  *next;
} job_t;

typedef struct {
    job_t *head;
} job_list_t;

void  job_list_init(job_list_t *jl);
job_t *job_add(job_list_t *jl, pid_t pid);
int   job_wait(job_list_t *jl, pid_t pid);   /* wait for specific pid, return exit code */
void  job_reap(job_list_t *jl);              /* non-blocking check for completed jobs */
pid_t job_last_bg(job_list_t *jl);           /* most recently added background pid */

#endif
