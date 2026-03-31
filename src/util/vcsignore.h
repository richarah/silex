/* vcsignore.h — .gitignore-style VCS filter for grep --vcs / find --vcs */

#ifndef SILEX_VCSIGNORE_H
#define SILEX_VCSIGNORE_H

typedef struct vcsignore vcsignore_t;

/*
 * Load .gitignore from `dir` (also checks parent directories up to fs root).
 * Always includes a built-in hardcoded skip list (.git/, node_modules/, etc.).
 * Returns NULL only on OOM; never returns NULL just because .gitignore is absent.
 */
vcsignore_t *vcsignore_load(const char *dir);

/*
 * Test whether `relpath` (relative to the dir passed to vcsignore_load)
 * should be ignored.
 * `is_dir`: 1 if the path is a directory, 0 if a regular file.
 * Returns 1 if ignored, 0 if not.
 */
int vcsignore_match(const vcsignore_t *ign, const char *relpath, int is_dir);

/*
 * Free all memory associated with ign.
 */
void vcsignore_free(vcsignore_t *ign);

/*
 * Quick check: is `name` (basename only) in the hardcoded VCS skip list?
 * Fast path used by grep_dir / find traversal before constructing full path.
 */
int vcsignore_skip_name(const char *name, int is_dir);

#endif /* SILEX_VCSIGNORE_H */
