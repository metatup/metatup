#ifndef TUP_METATUP_REPO_H
#define TUP_METATUP_REPO_H

int metatup_repo_materialize(const char *repo_root, const char *name, char **path_out, char **err);
int metatup_repo_materialize_all(const char *repo_root, char **err);
char *metatup_repo_rel_root_from_path(const char *workspace_root, const char *path);

#endif
