#include "metatup_repo.h"
#include <yaml.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct mtr_dep {
	char *name;
	char *git;
	char *github;
	char *path;
	char *branch;
};

struct mtr_file {
	struct mtr_dep *deps;
	int num_deps;
};

struct mtr_parser {
	yaml_parser_t parser;
	yaml_event_t event;
	const char *filename;
	char **err;
};

static void mtr_free_dep(struct mtr_dep *dep)
{
	free(dep->name);
	free(dep->git);
	free(dep->github);
	free(dep->path);
	free(dep->branch);
	memset(dep, 0, sizeof *dep);
}

static void mtr_free_file(struct mtr_file *mf)
{
	int x;
	for(x=0; x<mf->num_deps; x++)
		mtr_free_dep(&mf->deps[x]);
	free(mf->deps);
	memset(mf, 0, sizeof *mf);
}

static int mtr_next(struct mtr_parser *p)
{
	yaml_event_delete(&p->event);
	if(!yaml_parser_parse(&p->parser, &p->event)) {
		char buf[512];
		snprintf(buf, sizeof(buf), "%s:%lu:%lu: %s",
			 p->filename,
			 (unsigned long)p->parser.problem_mark.line + 1,
			 (unsigned long)p->parser.problem_mark.column + 1,
			 p->parser.problem ? p->parser.problem : "yaml parse error");
		*p->err = strdup(buf);
		if(!*p->err) {
			perror("strdup");
			return -1;
		}
		return -1;
	}
	return 0;
}

static int mtr_error(struct mtr_parser *p, const char *msg)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s:%lu:%lu: %s",
		 p->filename,
		 (unsigned long)p->event.start_mark.line + 1,
		 (unsigned long)p->event.start_mark.column + 1,
		 msg);
	*p->err = strdup(buf);
	if(!*p->err) {
		perror("strdup");
		return -1;
	}
	return -1;
}

static void mtr_warn_unknown(struct mtr_parser *p, const char *what, const char *key)
{
	fprintf(stderr, "tup warning: %s:%lu:%lu: ignoring unknown %s '%s'.\n",
		p->filename,
		(unsigned long)p->event.start_mark.line + 1,
		(unsigned long)p->event.start_mark.column + 1,
		what,
		key);
}

static int mtr_expect(struct mtr_parser *p, yaml_event_type_t type, const char *msg)
{
	if(p->event.type != type)
		return mtr_error(p, msg);
	return 0;
}

static int mtr_dup_scalar(struct mtr_parser *p, char **out)
{
	if(mtr_expect(p, YAML_SCALAR_EVENT, "expected scalar") < 0)
		return -1;
	*out = strdup((const char*)p->event.data.scalar.value);
	if(!*out) {
		perror("strdup");
		return -1;
	}
	return 0;
}

static int mtr_skip(struct mtr_parser *p)
{
	if(p->event.type == YAML_SCALAR_EVENT || p->event.type == YAML_ALIAS_EVENT)
		return mtr_next(p);
	if(p->event.type == YAML_SEQUENCE_START_EVENT) {
		if(mtr_next(p) < 0)
			return -1;
		while(p->event.type != YAML_SEQUENCE_END_EVENT) {
			if(mtr_skip(p) < 0)
				return -1;
		}
		return mtr_next(p);
	}
	if(p->event.type == YAML_MAPPING_START_EVENT) {
		if(mtr_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			if(mtr_skip(p) < 0)
				return -1;
			if(mtr_skip(p) < 0)
				return -1;
		}
		return mtr_next(p);
	}
	return mtr_error(p, "unable to skip yaml node");
}

static int mtr_parse_dep(struct mtr_parser *p, struct mtr_dep *dep)
{
	memset(dep, 0, sizeof *dep);
	if(mtr_expect(p, YAML_MAPPING_START_EVENT, "expected dependency mapping") < 0)
		return -1;
	if(mtr_next(p) < 0)
		return -1;
	while(p->event.type != YAML_MAPPING_END_EVENT) {
		char *key = NULL;
		char *value = NULL;
		if(mtr_dup_scalar(p, &key) < 0)
			return -1;
		if(mtr_next(p) < 0) {
			free(key);
			return -1;
		}
		if(strcmp(key, "name") == 0 ||
		   strcmp(key, "git") == 0 ||
		   strcmp(key, "github") == 0 ||
		   strcmp(key, "path") == 0 ||
		   strcmp(key, "branch") == 0) {
			if(mtr_dup_scalar(p, &value) < 0) {
				free(key);
				return -1;
			}
			if(strcmp(key, "name") == 0) {
				if(dep->name) {
					free(key);
					free(value);
					return mtr_error(p, "duplicate repository name");
				}
				dep->name = value;
			} else if(strcmp(key, "git") == 0) {
				if(dep->git) {
					free(key);
					free(value);
					return mtr_error(p, "duplicate repository git");
				}
				dep->git = value;
			} else if(strcmp(key, "github") == 0) {
				if(dep->github) {
					free(key);
					free(value);
					return mtr_error(p, "duplicate repository github");
				}
				dep->github = value;
			} else if(strcmp(key, "path") == 0) {
				if(dep->path) {
					free(key);
					free(value);
					return mtr_error(p, "duplicate repository path");
				}
				dep->path = value;
			} else {
				if(dep->branch) {
					free(key);
					free(value);
					return mtr_error(p, "duplicate repository branch");
				}
				dep->branch = value;
			}
			if(mtr_next(p) < 0) {
				free(key);
				return -1;
			}
		} else {
			mtr_warn_unknown(p, "repository key", key);
			free(key);
			if(mtr_skip(p) < 0)
				return -1;
			continue;
		}
		free(key);
	}
	if(!dep->name)
		return mtr_error(p, "repository dependency requires name");
	if((dep->git != NULL) + (dep->github != NULL) + (dep->path != NULL) != 1)
		return mtr_error(p, "repository dependency requires exactly one of git, github, or path");
	return mtr_next(p);
}

static int mtr_parse_dependencies(struct mtr_parser *p, struct mtr_file *mf)
{
	if(mtr_expect(p, YAML_SEQUENCE_START_EVENT, "expected dependencies sequence") < 0)
		return -1;
	if(mtr_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		struct mtr_dep dep;
		struct mtr_dep *tmp;
		if(mtr_parse_dep(p, &dep) < 0)
			return -1;
		tmp = realloc(mf->deps, sizeof(*tmp) * (mf->num_deps + 1));
		if(!tmp) {
			perror("realloc");
			mtr_free_dep(&dep);
			return -1;
		}
		mf->deps = tmp;
		mf->deps[mf->num_deps++] = dep;
	}
	return mtr_next(p);
}

static int mtr_load_file(const char *filename, struct mtr_file *mf, char **err)
{
	FILE *f;
	long len;
	char *buf = NULL;
	char *key = NULL;
	struct mtr_parser p;
	int rc = -1;

	memset(mf, 0, sizeof *mf);
	memset(&p, 0, sizeof p);
	*err = NULL;

	f = fopen(filename, "r");
	if(!f) {
		size_t msglen = strlen(filename) + strlen(strerror(errno)) + 32;
		*err = malloc(msglen);
		if(!*err) {
			perror("malloc");
			return -1;
		}
		snprintf(*err, msglen, "unable to open %s: %s", filename, strerror(errno));
		return -1;
	}
	if(fseek(f, 0, SEEK_END) < 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0) {
		perror(filename);
		fclose(f);
		return -1;
	}
	buf = malloc(len + 1);
	if(!buf) {
		perror("malloc");
		fclose(f);
		return -1;
	}
	if(len > 0 && fread(buf, 1, len, f) != (size_t)len) {
		perror(filename);
		free(buf);
		fclose(f);
		return -1;
	}
	buf[len] = 0;
	fclose(f);

	if(!yaml_parser_initialize(&p.parser)) {
		*err = strdup("unable to initialize yaml parser");
		if(!*err)
			perror("strdup");
		free(buf);
		return -1;
	}
	p.filename = filename;
	p.err = err;
	yaml_parser_set_input_string(&p.parser, (const unsigned char*)buf, len);

	if(mtr_next(&p) < 0)
		goto out;
	if(mtr_expect(&p, YAML_STREAM_START_EVENT, "expected stream start") < 0)
		goto out;
	if(mtr_next(&p) < 0)
		goto out;
	if(mtr_expect(&p, YAML_DOCUMENT_START_EVENT, "expected document start") < 0)
		goto out;
	if(mtr_next(&p) < 0)
		goto out;
	if(mtr_expect(&p, YAML_MAPPING_START_EVENT, "expected top-level mapping") < 0)
		goto out;
	if(mtr_next(&p) < 0)
		goto out;
	while(p.event.type != YAML_MAPPING_END_EVENT) {
		if(mtr_dup_scalar(&p, &key) < 0)
			goto out;
		if(mtr_next(&p) < 0)
			goto out;
		if(strcmp(key, "meta") == 0) {
			if(mtr_skip(&p) < 0)
				goto out;
		} else if(strcmp(key, "dependencies") == 0) {
			if(mtr_parse_dependencies(&p, mf) < 0)
				goto out;
		} else {
			mtr_warn_unknown(&p, "top-level key", key);
			if(mtr_skip(&p) < 0)
				goto out;
		}
		free(key);
		key = NULL;
	}
	rc = 0;
out:
	free(key);
	free(buf);
	yaml_event_delete(&p.event);
	yaml_parser_delete(&p.parser);
	if(rc < 0)
		mtr_free_file(mf);
	return rc;
}

static char *mtr_join(const char *a, const char *b)
{
	size_t len = strlen(a) + strlen(b) + 2;
	char *out = malloc(len);
	if(!out) {
		perror("malloc");
		return NULL;
	}
	snprintf(out, len, "%s/%s", a, b);
	return out;
}

static int mtr_mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	char *p;

	if(strlen(path) >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		perror("mkdir");
		return -1;
	}
	strcpy(tmp, path);
	for(p = tmp + 1; *p; p++) {
		if(*p != '/')
			continue;
		*p = 0;
		if(mkdir(tmp, 0777) < 0 && errno != EEXIST) {
			perror(tmp);
			return -1;
		}
		*p = '/';
	}
	if(mkdir(tmp, 0777) < 0 && errno != EEXIST) {
		perror(tmp);
		return -1;
	}
	return 0;
}

static int mtr_run_git_clone(const char *url, const char *branch, const char *target, char **err)
{
	pid_t pid;
	int status;

	pid = fork();
	if(pid < 0) {
		perror("fork");
		return -1;
	}
	if(pid == 0) {
		if(branch) {
			execlp("git", "git", "clone", "--branch", branch, "--single-branch", url, target, NULL);
		} else {
			execlp("git", "git", "clone", url, target, NULL);
		}
		perror("git");
		_exit(127);
	}
	if(waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return -1;
	}
	if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	*err = malloc(strlen(url) + strlen(target) + 128);
	if(!*err) {
		perror("malloc");
		return -1;
	}
	snprintf(*err, strlen(url) + strlen(target) + 128,
		 "failed to clone repository '%s' into '%s'", url, target);
	return -1;
}

static struct mtr_dep *mtr_find_dep(struct mtr_file *mf, const char *name)
{
	int x;
	for(x=0; x<mf->num_deps; x++) {
		if(strcmp(mf->deps[x].name, name) == 0)
			return &mf->deps[x];
	}
	return NULL;
}

char *metatup_repo_rel_root_from_path(const char *workspace_root, const char *path)
{
	char *copy;
	char *saveptr = NULL;
	char *tok;
	char **parts = NULL;
	int num_parts = 0;
	int repo_end = -1;
	int cap = 0;
	int x;
	size_t len = 0;
	char *out;

	struct mtr_file mf;
	char *config = NULL;
	char *abs_workspace = NULL;
	char *abs_path = NULL;
	int rc;

	if(!path || strcmp(path, ".") == 0)
		return strdup("");
	copy = strdup(path);
	if(!copy) {
		perror("strdup");
		return NULL;
	}
	for(tok = strtok_r(copy, "/", &saveptr); tok; tok = strtok_r(NULL, "/", &saveptr)) {
		char **tmp;
		if(num_parts == cap) {
			cap = cap ? cap * 2 : 8;
			tmp = realloc(parts, sizeof(*tmp) * cap);
			if(!tmp) {
				perror("realloc");
				free(parts);
				free(copy);
				return NULL;
			}
			parts = tmp;
		}
		parts[num_parts++] = tok;
	}
	for(x=0; x+2<num_parts; x++) {
		if(strcmp(parts[x], ".metatup") == 0 &&
		   strcmp(parts[x+1], "repos") == 0)
			repo_end = x + 2;
	}
	if(repo_end < 0) {
		if(!workspace_root)
			goto no_repo_match;
		abs_workspace = realpath(workspace_root, NULL);
		if(!abs_workspace)
			goto no_repo_match;
		{
			size_t plen = strlen(abs_workspace) + strlen(path) + 2;
			char *joined = malloc(plen);
			if(!joined) {
				perror("malloc");
				free(abs_workspace);
				free(parts);
				free(copy);
				return NULL;
			}
			snprintf(joined, plen, "%s/%s", abs_workspace, path);
			abs_path = realpath(joined, NULL);
			free(joined);
		}
		if(!abs_path)
			goto no_repo_match;
		config = mtr_join(abs_workspace, "MetaTupRepo.yaml");
		if(!config)
			goto no_repo_match;
		rc = access(config, F_OK);
		if(rc != 0)
			goto no_repo_match;
		if(mtr_load_file(config, &mf, &config) < 0)
			goto no_repo_match_loaded;
		for(x=0; x<mf.num_deps; x++) {
			if(mf.deps[x].path) {
				char *dep_abs = NULL;
				char *joined;
				size_t jlen;
				if(mf.deps[x].path[0] == '/') {
					dep_abs = realpath(mf.deps[x].path, NULL);
				} else {
					jlen = strlen(abs_workspace) + strlen(mf.deps[x].path) + 2;
					joined = malloc(jlen);
					if(!joined) {
						perror("malloc");
						mtr_free_file(&mf);
						free(config);
						free(abs_workspace);
						free(abs_path);
						free(parts);
						free(copy);
						return NULL;
					}
					snprintf(joined, jlen, "%s/%s", abs_workspace, mf.deps[x].path);
					dep_abs = realpath(joined, NULL);
					free(joined);
				}
				if(dep_abs) {
					size_t dlen = strlen(dep_abs);
					if(strcmp(abs_path, dep_abs) == 0 ||
					   (strncmp(abs_path, dep_abs, dlen) == 0 && abs_path[dlen] == '/')) {
						char *repo_root_rel = strdup(mf.deps[x].path);
						free(dep_abs);
						mtr_free_file(&mf);
						free(config);
						free(abs_workspace);
						free(abs_path);
						free(parts);
						free(copy);
						if(!repo_root_rel) {
							perror("strdup");
							return NULL;
						}
						return repo_root_rel;
					}
					free(dep_abs);
				}
			}
		}
		mtr_free_file(&mf);
no_repo_match_loaded:
		free(config);
no_repo_match:
		free(abs_workspace);
		free(abs_path);
		free(parts);
		free(copy);
		return strdup("");
	}
	for(x=0; x<=repo_end; x++)
		len += strlen(parts[x]) + (x == 0 ? 0 : 1);
	out = malloc(len + 1);
	if(!out) {
		perror("malloc");
		free(parts);
		free(copy);
		return NULL;
	}
	out[0] = 0;
	for(x=0; x<=repo_end; x++) {
		if(x > 0)
			strcat(out, "/");
		strcat(out, parts[x]);
	}
	free(parts);
	free(copy);
	return out;
}

int metatup_repo_materialize(const char *repo_root, const char *name, char **path_out, char **err)
{
	struct mtr_file mf;
	struct mtr_dep *dep;
	char *config = NULL;
	char *repos_dir = NULL;
	char *target = NULL;
	int rc = -1;

	*path_out = NULL;
	*err = NULL;
	config = mtr_join(repo_root, "MetaTupRepo.yaml");
	repos_dir = mtr_join(repo_root, ".metatup/repos");
	target = repos_dir ? mtr_join(repos_dir, name) : NULL;
	if(!config || !repos_dir || !target)
		goto out;
	if(mtr_load_file(config, &mf, err) < 0)
		goto out;
	dep = mtr_find_dep(&mf, name);
	if(!dep) {
		*err = malloc(strlen(name) + strlen(config) + 64);
		if(!*err) {
			perror("malloc");
			mtr_free_file(&mf);
			goto out;
		}
		snprintf(*err, strlen(name) + strlen(config) + 64,
			 "unable to find repository '%s' in %s", name, config);
		mtr_free_file(&mf);
		goto out;
	}
	if(mtr_mkdir_p(repos_dir) < 0) {
		mtr_free_file(&mf);
		goto out;
	}
	if(dep->path) {
		char *joined = NULL;
		char *source = NULL;

		if(dep->path[0] == '/') {
			source = strdup(dep->path);
		} else {
			joined = mtr_join(repo_root, dep->path);
			if(joined)
				source = realpath(joined, NULL);
			if(!source && joined)
				source = strdup(joined);
		}
		free(joined);
		if(!source) {
			perror("strdup");
			mtr_free_file(&mf);
			goto out;
		}
		free(target);
		target = source;
	} else {
		char *url;
		struct stat st;
		if(dep->git) {
			url = strdup(dep->git);
		} else {
			size_t len = strlen(dep->github) + strlen("https://github.com/.git") + 1;
			url = malloc(len);
			if(url)
				snprintf(url, len, "https://github.com/%s.git", dep->github);
		}
		if(!url) {
			perror("malloc");
			mtr_free_file(&mf);
			goto out;
		}
		if(stat(target, &st) < 0) {
			if(errno != ENOENT || mtr_run_git_clone(url, dep->branch, target, err) < 0) {
				free(url);
				mtr_free_file(&mf);
				goto out;
			}
		}
		free(url);
	}
	*path_out = strdup(target);
	if(!*path_out) {
		perror("strdup");
		mtr_free_file(&mf);
		goto out;
	}
	mtr_free_file(&mf);
	rc = 0;
out:
	free(config);
	free(repos_dir);
	free(target);
	return rc;
}

int metatup_repo_materialize_all(const char *repo_root, char **err)
{
	struct mtr_file mf;
	char *config = NULL;
	int x;

	*err = NULL;
	config = mtr_join(repo_root, "MetaTupRepo.yaml");
	if(!config)
		return -1;
	if(access(config, F_OK) != 0) {
		free(config);
		return 0;
	}
	if(mtr_load_file(config, &mf, err) < 0) {
		free(config);
		return -1;
	}
	for(x=0; x<mf.num_deps; x++) {
		char *path = NULL;
		if(mf.deps[x].path)
			continue;
		if(metatup_repo_materialize(repo_root, mf.deps[x].name, &path, err) < 0) {
			mtr_free_file(&mf);
			free(config);
			return -1;
		}
		free(path);
	}
	mtr_free_file(&mf);
	free(config);
	return 0;
}
