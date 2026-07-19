/* job.c — job control: background pipelines, job table, and waiting */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "job.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int status_to_code(int status)
{
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status))  return 128 + WSTOPSIG(status);
    return 1;
}

void job_list_init(job_list_t *jl)
{
    jl->head     = NULL;
    jl->current  = 0;
    jl->previous = 0;
    jl->next_id  = 1;
}

void job_list_free(job_list_t *jl)
{
    job_t *j = jl->head;
    while (j) {
        job_t *next = j->next;
        free(j->command);
        free(j);
        j = next;
    }
    jl->head = NULL;
}

/* Append to the tail so `jobs` lists in ascending job-number order. */
static void job_append(job_list_t *jl, job_t *j)
{
    j->next = NULL;
    if (!jl->head) {
        jl->head = j;
        return;
    }
    job_t *t = jl->head;
    while (t->next)
        t = t->next;
    t->next = j;
}

/* Make `id` the current job; the old current becomes previous. */
static void job_set_current(job_list_t *jl, int id)
{
    if (jl->current != id) {
        jl->previous = jl->current;
        jl->current  = id;
    }
}

job_t *job_register(job_list_t *jl, pid_t pid, pid_t pgid, const char *command)
{
    job_t *j = malloc(sizeof(job_t));
    if (!j)
        return NULL;
    j->id       = jl->next_id++;
    j->pid      = pid;
    j->pgid     = pgid ? pgid : pid;
    j->status   = 0;
    j->state    = JOB_RUNNING;
    j->notified = 0;
    j->command  = command ? strdup(command) : NULL;
    j->next     = NULL;
    job_append(jl, j);
    job_set_current(jl, j->id);
    return j;
}

job_t *job_add(job_list_t *jl, pid_t pid)
{
    return job_register(jl, pid, pid, NULL);
}

job_t *job_find_by_id(job_list_t *jl, int id)
{
    for (job_t *j = jl->head; j; j = j->next)
        if (j->id == id)
            return j;
    return NULL;
}

job_t *job_find_by_pid(job_list_t *jl, pid_t pid)
{
    for (job_t *j = jl->head; j; j = j->next)
        if (j->pid == pid)
            return j;
    return NULL;
}

job_t *job_current(job_list_t *jl)
{
    job_t *j = job_find_by_id(jl, jl->current);
    if (j) return j;
    /* Fall back to the highest-numbered job if the recorded current is gone. */
    job_t *best = NULL;
    for (job_t *k = jl->head; k; k = k->next)
        if (!best || k->id > best->id)
            best = k;
    return best;
}

job_t *job_previous(job_list_t *jl)
{
    job_t *j = job_find_by_id(jl, jl->previous);
    return j ? j : job_current(jl);
}

void job_remove(job_list_t *jl, job_t *j)
{
    job_t **pp = &jl->head;
    while (*pp) {
        if (*pp == j) {
            *pp = j->next;
            if (jl->current == j->id) {
                /* Promote the previous job to current. */
                jl->current  = jl->previous;
                jl->previous = 0;
            } else if (jl->previous == j->id) {
                jl->previous = 0;
            }
            free(j->command);
            free(j);
            /* Reset the numbering to 1 once the table drains, like bash/dash. */
            if (!jl->head)
                jl->next_id = 1;
            return;
        }
        pp = &(*pp)->next;
    }
}

int job_wait(job_list_t *jl, pid_t pid)
{
    job_t *job = NULL;
    for (job_t *j = jl->head; j != NULL; j = j->next)
        if (j->pid == pid) { job = j; break; }

    /* Already reaped (e.g. by job_reap() during `jobs`): return its status
     * rather than call waitpid() again -- a second wait would get ECHILD. */
    if (job && job->state == JOB_DONE)
        return status_to_code(job->status);

    int status = 0;
    pid_t ret;
    do {
        ret = waitpid(pid, &status, 0);
    } while (ret == -1 && errno == EINTR);   /* ONLY retry on EINTR */

    if (ret == -1)
        /* ECHILD or similar: the process is gone and was already reaped
         * elsewhere. Report whatever we last recorded, or success. */
        return job ? status_to_code(job->status) : 0;

    if (job) {
        job->status = status;
        job->state  = WIFSTOPPED(status) ? JOB_STOPPED : JOB_DONE;
    }
    return status_to_code(status);
}

void job_reap(job_list_t *jl)
{
    int status;
    pid_t pid;

    /* WUNTRACED so a job stopped by SIGTSTP is noticed, not just terminated
     * ones. WCONTINUED would need the corresponding request when resuming. */
    for (;;) {
        pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
        if (pid <= 0)
            break;

        for (job_t *j = jl->head; j != NULL; j = j->next) {
            if (j->pid == pid) {
                j->status = status;
                if (WIFSTOPPED(status))
                    j->state = JOB_STOPPED;
                else
                    j->state = JOB_DONE;
                break;
            }
        }
    }
}

pid_t job_last_bg(job_list_t *jl)
{
    /* Most recently registered job = the one with the highest id. */
    job_t *best = NULL;
    for (job_t *j = jl->head; j; j = j->next)
        if (!best || j->id > best->id)
            best = j;
    return best ? best->pid : 0;
}
