/* applets.h — applet declarations and registration table for matchbox */

#ifndef MATCHBOX_APPLETS_H
#define MATCHBOX_APPLETS_H

/* Applet function prototype: same signature as main() */
typedef int (*applet_fn_t)(int argc, char **argv);

typedef struct {
    const char    *name;
    applet_fn_t    fn;
    const char    *usage; /* short usage line, printed by --list */
} applet_t;

/* Phase 1 applets */
int applet_cp(int argc, char **argv);
int applet_echo(int argc, char **argv);
int applet_mkdir(int argc, char **argv);

/* Phase 2: shell */
int applet_sh(int argc, char **argv);

/* Phase 3: core builtins */
int applet_cat(int argc, char **argv);
int applet_chmod(int argc, char **argv);
int applet_mv(int argc, char **argv);
int applet_rm(int argc, char **argv);
int applet_ln(int argc, char **argv);
int applet_touch(int argc, char **argv);
int applet_head(int argc, char **argv);
int applet_tail(int argc, char **argv);
int applet_wc(int argc, char **argv);
int applet_sort(int argc, char **argv);
int applet_grep(int argc, char **argv);
int applet_sed(int argc, char **argv);
int applet_find(int argc, char **argv);
int applet_xargs(int argc, char **argv);
int applet_basename(int argc, char **argv);
int applet_dirname(int argc, char **argv);
int applet_readlink(int argc, char **argv);
int applet_stat(int argc, char **argv);
int applet_date(int argc, char **argv);
int applet_printf(int argc, char **argv);
int applet_install(int argc, char **argv);
int applet_tr(int argc, char **argv);
int applet_cut(int argc, char **argv);

/* Global applet table (defined in main.c) */
extern const applet_t applet_table[];

/* Find applet by name; returns NULL if not found */
const applet_t *find_applet(const char *name);

#endif /* MATCHBOX_APPLETS_H */
