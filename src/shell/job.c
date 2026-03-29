#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "job.h"

#include <stdlib.h>
#include <sys/wait.h>

void job_list_init(job_list_t *jl)
{
    jl->head = NULL;
}

job_t *job_add(job_list_t *jl, pid_t pid)
{
    job_t *j = malloc(sizeof(job_t));
    if (!j)
        return NULL;
    j->pid    = pid;
    j->status = 0;
    j->done   = 0;
    j->next   = jl->head;
    jl->head  = j;
    return j;
}

int job_wait(job_list_t *jl, pid_t pid)
{
    int status = 0;
    pid_t ret;

    do {
        ret = waitpid(pid, &status, 0);
    } while (ret == -1);  /* retry on EINTR */

    /* Update the matching job entry */
    for (job_t *j = jl->head; j != NULL; j = j->next) {
        if (j->pid == pid) {
            j->status = status;
            j->done   = 1;
            break;
        }
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

void job_reap(job_list_t *jl)
{
    int status;
    pid_t pid;

    for (;;) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        for (job_t *j = jl->head; j != NULL; j = j->next) {
            if (j->pid == pid) {
                j->status = status;
                j->done   = 1;
                break;
            }
        }
    }
}

pid_t job_last_bg(job_list_t *jl)
{
    if (!jl->head)
        return 0;
    return jl->head->pid;
}
