#include "metatup.h"
#include "metatup_repo.h"
#include "config.h"
#include "tupbuild.h"
#include <yaml.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct mt_kv {
	char *key;
	char *value;
};

struct mt_bind {
	char **from;
	int num_from;
	char *to;
	char *value;
	struct mt_kv *cases;
	int num_cases;
	char *default_case;
};

enum mt_condition_type {
	MT_COND_PROFILE_ENABLED,
	MT_COND_ARGS_EQUAL,
	MT_COND_AND,
	MT_COND_OR,
	MT_COND_NOT,
};

struct mt_condition {
	enum mt_condition_type type;
	char *profile;
	struct mt_kv *args_equal;
	int num_args_equal;
	struct mt_condition *children;
	int num_children;
};

struct mt_dep {
	char *name;
	struct mt_bind *binds;
	int num_binds;
	struct mt_condition *require_if;
	int num_require_if;
};

struct mt_profile {
	char *name;
	struct mt_kv *set;
	int num_set;
};

struct mt_component {
	char *name;
	char *tupfile;
	char *function;
	struct mt_bind *binds;
	int num_binds;
	struct mt_dep *deps;
	int num_deps;
};

struct mt_file {
	struct mt_kv *defaults;
	int num_defaults;
	int auto_compiledb;
	int auto_compiledb_set;
	struct mt_profile *profiles;
	int num_profiles;
	struct mt_component *components;
	int num_components;
};

struct mt_generated_build {
	char *name;
	char *tupfile;
	char *function;
	char *builddir;
	char *profile;
	struct mt_kv *args;
	int num_args;
	struct tupbuild_dep *depends;
	int num_depends;
	struct tupbuild_dist *dists;
	int num_dists;
	char *identity;
};

struct mt_dep_result {
	struct mt_generated_build **builds;
	int num_builds;
};

struct mt_state {
	char cwd[PATH_MAX];
	char root[PATH_MAX];
	char *root_meta_path;
	char *component_name;
	char *root_build_name;
	char *build_name_suffix;
	char *override_builddir;
	char *profile;
	int strict;
	int auto_compiledb;
	struct mt_kv *cli_args;
	int num_cli_args;
	struct tupbuild_dist *cli_dists;
	int num_cli_dists;
	struct tupbuild_file existing_tb;
	int have_existing_tb;
	struct mt_generated_build *generated;
	int num_generated;
};

struct mt_yaml_parser {
	yaml_parser_t parser;
	yaml_event_t event;
	const char *filename;
	char **err;
};

struct mt_label {
	char *repo;
	char *package;
	char *component;
};

static int mt_parse_label(const char *text, const char *current_package, struct mt_label *label, char **err);
static int mt_make_dep_build_name(const char *root_build_name, const char *repo_root_rel,
				  const char *package, const char *component, char **out);

static int mt_parse_quoted_string(const char **sp, char **out)
{
	const char *s = *sp;
	const char *start;

	if(*s != '"')
		return -1;
	s++;
	start = s;
	while(*s && *s != '"') {
		if(*s == '\\' && s[1] != 0)
			s += 2;
		else
			s++;
	}
	if(*s != '"')
		return -1;
	*out = strndup(start, s - start);
	if(!*out) {
		perror("strndup");
		return -1;
	}
	*sp = s + 1;
	return 0;
}

static char *mt_yaml_quote(const char *s)
{
	size_t cap = strlen(s) * 2 + 1;
	size_t len = 0;
	char *out = malloc(cap);

	if(!out) {
		perror("malloc");
		return NULL;
	}
	while(*s) {
		if(*s == '"' || *s == '\\')
			out[len++] = '\\';
		out[len++] = *s++;
	}
	out[len] = 0;
	return out;
}

static void mt_free_kvs(struct mt_kv *kvs, int count)
{
	int x;
	for(x=0; x<count; x++) {
		free(kvs[x].key);
		free(kvs[x].value);
	}
	free(kvs);
}

static void mt_free_binds(struct mt_bind *binds, int count)
{
	int x;
	int y;
	for(x=0; x<count; x++) {
		for(y=0; y<binds[x].num_from; y++)
			free(binds[x].from[y]);
		free(binds[x].from);
		free(binds[x].to);
		free(binds[x].value);
		mt_free_kvs(binds[x].cases, binds[x].num_cases);
		free(binds[x].default_case);
	}
	free(binds);
}

static void mt_free_conditions(struct mt_condition *conds, int count)
{
	int x;
	for(x=0; x<count; x++) {
		free(conds[x].profile);
		mt_free_kvs(conds[x].args_equal, conds[x].num_args_equal);
		mt_free_conditions(conds[x].children, conds[x].num_children);
	}
	free(conds);
}

static void mt_free_deps(struct mt_dep *deps, int count)
{
	int x;
	for(x=0; x<count; x++) {
		free(deps[x].name);
		mt_free_binds(deps[x].binds, deps[x].num_binds);
		mt_free_conditions(deps[x].require_if, deps[x].num_require_if);
	}
	free(deps);
}

static void mt_free_profiles(struct mt_profile *profiles, int count)
{
	int x;
	for(x=0; x<count; x++) {
		free(profiles[x].name);
		mt_free_kvs(profiles[x].set, profiles[x].num_set);
	}
	free(profiles);
}

static void mt_cleanup_profile(struct mt_profile *profile)
{
	free(profile->name);
	mt_free_kvs(profile->set, profile->num_set);
	memset(profile, 0, sizeof *profile);
}

static void mt_free_file(struct mt_file *mf)
{
	int x;
	mt_free_kvs(mf->defaults, mf->num_defaults);
	mt_free_profiles(mf->profiles, mf->num_profiles);
	for(x=0; x<mf->num_components; x++) {
		free(mf->components[x].name);
		free(mf->components[x].tupfile);
		free(mf->components[x].function);
		mt_free_binds(mf->components[x].binds, mf->components[x].num_binds);
		mt_free_deps(mf->components[x].deps, mf->components[x].num_deps);
	}
	free(mf->components);
	memset(mf, 0, sizeof *mf);
}

static void mt_free_generated_build(struct mt_generated_build *b)
{
	int x;
	free(b->name);
	free(b->tupfile);
	free(b->function);
	free(b->builddir);
	free(b->profile);
	free(b->identity);
	mt_free_kvs(b->args, b->num_args);
	for(x=0; x<b->num_depends; x++) {
		free(b->depends[x].build);
		free(b->depends[x].as);
	}
	free(b->depends);
	for(x=0; x<b->num_dists; x++) {
		free(b->dists[x].from_return);
		free(b->dists[x].path);
	}
	free(b->dists);
	memset(b, 0, sizeof *b);
}

static void mt_free_dep_result(struct mt_dep_result *dr)
{
	free(dr->builds);
	dr->builds = NULL;
	dr->num_builds = 0;
}

static void mt_free_state(struct mt_state *st)
{
	int x;
	free(st->root_meta_path);
	free(st->component_name);
	free(st->root_build_name);
	free(st->build_name_suffix);
	free(st->override_builddir);
	free(st->profile);
	mt_free_kvs(st->cli_args, st->num_cli_args);
	for(x=0; x<st->num_cli_dists; x++) {
		free(st->cli_dists[x].from_return);
		free(st->cli_dists[x].path);
	}
	free(st->cli_dists);
	if(st->have_existing_tb)
		tupbuild_free(&st->existing_tb);
	for(x=0; x<st->num_generated; x++)
		mt_free_generated_build(&st->generated[x]);
	free(st->generated);
}

static int mt_yaml_next(struct mt_yaml_parser *p)
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

static int mt_yaml_error(struct mt_yaml_parser *p, const char *msg)
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

static void mt_yaml_warn_unknown(struct mt_yaml_parser *p, const char *what, const char *key)
{
	fprintf(stderr, "tup warning: %s:%lu:%lu: ignoring unknown %s '%s'.\n",
		p->filename,
		(unsigned long)p->event.start_mark.line + 1,
		(unsigned long)p->event.start_mark.column + 1,
		what,
		key);
}

static int mt_yaml_expect(struct mt_yaml_parser *p, yaml_event_type_t type, const char *msg)
{
	if(p->event.type != type)
		return mt_yaml_error(p, msg);
	return 0;
}

static int mt_yaml_dup_scalar(struct mt_yaml_parser *p, char **out)
{
	if(mt_yaml_expect(p, YAML_SCALAR_EVENT, "expected scalar") < 0)
		return -1;
	*out = strdup((const char*)p->event.data.scalar.value);
	if(!*out) {
		perror("strdup");
		return -1;
	}
	return 0;
}

static int mt_yaml_skip(struct mt_yaml_parser *p)
{
	if(p->event.type == YAML_SCALAR_EVENT || p->event.type == YAML_ALIAS_EVENT)
		return mt_yaml_next(p);
	if(p->event.type == YAML_SEQUENCE_START_EVENT) {
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_SEQUENCE_END_EVENT) {
			if(mt_yaml_skip(p) < 0)
				return -1;
		}
		return mt_yaml_next(p);
	}
	if(p->event.type == YAML_MAPPING_START_EVENT) {
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			if(mt_yaml_skip(p) < 0)
				return -1;
			if(mt_yaml_skip(p) < 0)
				return -1;
		}
		return mt_yaml_next(p);
	}
	return mt_yaml_error(p, "unable to skip yaml node");
}

static int mt_add_kv_raw(struct mt_kv **kvs, int *count, char *key, char *value)
{
	struct mt_kv *tmp = realloc(*kvs, sizeof(**kvs) * (*count + 1));
	if(!tmp) {
		perror("realloc");
		return -1;
	}
	*kvs = tmp;
	(*kvs)[*count].key = key;
	(*kvs)[*count].value = value;
	(*count)++;
	return 0;
}

static int mt_set_kv(struct mt_kv **kvs, int *count, const char *key, const char *value)
{
	int x;
	for(x=0; x<*count; x++) {
		if(strcmp((*kvs)[x].key, key) == 0) {
			char *dup = strdup(value);
			if(!dup) {
				perror("strdup");
				return -1;
			}
			free((*kvs)[x].value);
			(*kvs)[x].value = dup;
			return 0;
		}
	}
	return mt_add_kv_raw(kvs, count, strdup(key), strdup(value));
}

static const char *mt_get_kv(struct mt_kv *kvs, int count, const char *key)
{
	int x;
	for(x=0; x<count; x++) {
		if(strcmp(kvs[x].key, key) == 0)
			return kvs[x].value;
	}
	return NULL;
}

static int mt_clone_kvs(struct mt_kv *src, int num_src, struct mt_kv **dst, int *num_dst)
{
	int x;
	for(x=0; x<num_src; x++) {
		if(mt_set_kv(dst, num_dst, src[x].key, src[x].value) < 0)
			return -1;
	}
	return 0;
}

static int mt_clone_kvs_except(struct mt_kv *src, int num_src, struct mt_kv **dst, int *num_dst, const char *skip_key)
{
	int x;
	for(x=0; x<num_src; x++) {
		if(skip_key && strcmp(src[x].key, skip_key) == 0)
			continue;
		if(mt_set_kv(dst, num_dst, src[x].key, src[x].value) < 0)
			return -1;
	}
	return 0;
}

static int mt_is_from_expr_value(const char *value)
{
	while(value && isspace((unsigned char)*value))
		value++;
	return value && strncmp(value, "$(from", 6) == 0;
}

static int mt_clone_non_from_kvs(struct mt_kv *src, int num_src, struct mt_kv **dst, int *num_dst)
{
	int x;
	for(x=0; x<num_src; x++) {
		if(mt_is_from_expr_value(src[x].value))
			continue;
		if(mt_set_kv(dst, num_dst, src[x].key, src[x].value) < 0)
			return -1;
	}
	return 0;
}

static int mt_parse_string_seq(struct mt_yaml_parser *p, char ***arr, int *count)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		char *value = NULL;
		char **tmp;
		if(mt_yaml_dup_scalar(p, &value) < 0)
			return -1;
		tmp = realloc(*arr, sizeof(**arr) * (*count + 1));
		if(!tmp) {
			perror("realloc");
			free(value);
			return -1;
		}
		*arr = tmp;
		(*arr)[*count] = value;
		(*count)++;
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_parse_string_map(struct mt_yaml_parser *p, struct mt_kv **kvs, int *count)
{
	if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected mapping") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_MAPPING_END_EVENT) {
		char *key = NULL;
		char *value = NULL;
		if(mt_yaml_dup_scalar(p, &key) < 0)
			return -1;
		if(mt_yaml_next(p) < 0) {
			free(key);
			return -1;
		}
		if(mt_yaml_dup_scalar(p, &value) < 0) {
			free(key);
			return -1;
		}
		if(mt_add_kv_raw(kvs, count, key, value) < 0) {
			free(key);
			free(value);
			return -1;
		}
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_parse_bind_cases(struct mt_yaml_parser *p, struct mt_bind *bind)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected bind case sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		char *when = NULL;
		char *then = NULL;
		if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected bind case mapping") < 0)
			return -1;
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			char *key = NULL;
			if(mt_yaml_dup_scalar(p, &key) < 0)
				return -1;
			if(mt_yaml_next(p) < 0) {
				free(key);
				return -1;
			}
			if(strcmp(key, "when") == 0) {
				if(mt_yaml_dup_scalar(p, &when) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "then") == 0) {
				if(mt_yaml_dup_scalar(p, &then) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "default") == 0) {
				if(mt_yaml_dup_scalar(p, &bind->default_case) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else {
				mt_yaml_warn_unknown(p, "bind case key", key);
				free(key);
				if(mt_yaml_skip(p) < 0) {
					free(when);
					free(then);
					return -1;
				}
				continue;
			}
			free(key);
		}
		if((when && !then) || (!when && then)) {
			free(when);
			free(then);
			return mt_yaml_error(p, "bind case requires both when and then");
		}
		if(when) {
			if(mt_add_kv_raw(&bind->cases, &bind->num_cases, when, then) < 0) {
				free(when);
				free(then);
				return -1;
			}
		}
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_parse_conditions(struct mt_yaml_parser *p, struct mt_condition **conds, int *count);

static int mt_parse_condition(struct mt_yaml_parser *p, struct mt_condition *cond)
{
	memset(cond, 0, sizeof *cond);
	if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected condition mapping") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_MAPPING_END_EVENT) {
		char *key = NULL;
		if(mt_yaml_dup_scalar(p, &key) < 0)
			return -1;
		if(mt_yaml_next(p) < 0) {
			free(key);
			return -1;
		}
		if(strcmp(key, "profile_enabled") == 0) {
			cond->type = MT_COND_PROFILE_ENABLED;
			if(mt_yaml_dup_scalar(p, &cond->profile) < 0) {
				free(key);
				return -1;
			}
			if(mt_yaml_next(p) < 0) {
				free(key);
				return -1;
			}
		} else if(strcmp(key, "args_equal") == 0) {
			cond->type = MT_COND_ARGS_EQUAL;
			if(mt_parse_string_map(p, &cond->args_equal, &cond->num_args_equal) < 0) {
				free(key);
				return -1;
			}
		} else if(strcmp(key, "and") == 0) {
			cond->type = MT_COND_AND;
			if(mt_parse_conditions(p, &cond->children, &cond->num_children) < 0) {
				free(key);
				return -1;
			}
		} else if(strcmp(key, "or") == 0) {
			cond->type = MT_COND_OR;
			if(mt_parse_conditions(p, &cond->children, &cond->num_children) < 0) {
				free(key);
				return -1;
			}
		} else if(strcmp(key, "not") == 0) {
			cond->type = MT_COND_NOT;
			cond->children = malloc(sizeof(*cond->children));
			if(!cond->children) {
				perror("malloc");
				free(key);
				return -1;
			}
			cond->num_children = 1;
			if(mt_parse_condition(p, &cond->children[0]) < 0) {
				free(key);
				return -1;
			}
		} else {
			mt_yaml_warn_unknown(p, "require_if condition key", key);
			free(key);
			if(mt_yaml_skip(p) < 0)
				return -1;
			continue;
		}
		free(key);
	}
	if((cond->type == MT_COND_PROFILE_ENABLED && !cond->profile) ||
	   (cond->type == MT_COND_ARGS_EQUAL && cond->num_args_equal == 0) ||
	   ((cond->type == MT_COND_AND || cond->type == MT_COND_OR || cond->type == MT_COND_NOT) && cond->num_children == 0)) {
		free(cond->profile);
		mt_free_kvs(cond->args_equal, cond->num_args_equal);
		mt_free_conditions(cond->children, cond->num_children);
		memset(cond, 0, sizeof *cond);
		return mt_yaml_error(p, "invalid require_if condition");
	}
	return mt_yaml_next(p);
}

static int mt_parse_conditions(struct mt_yaml_parser *p, struct mt_condition **conds, int *count)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected require_if sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		struct mt_condition cond;
		struct mt_condition *tmp;
		if(mt_parse_condition(p, &cond) < 0)
			return -1;
		tmp = realloc(*conds, sizeof(**conds) * (*count + 1));
		if(!tmp) {
			perror("realloc");
			mt_free_conditions(&cond, 1);
			return -1;
		}
		*conds = tmp;
		(*conds)[*count] = cond;
		(*count)++;
	}
	return mt_yaml_next(p);
}

static int mt_parse_binds(struct mt_yaml_parser *p, struct mt_bind **binds, int *count)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected binds sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		struct mt_bind bind;
		struct mt_bind *tmp;
		memset(&bind, 0, sizeof bind);
		if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected bind mapping") < 0)
			return -1;
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			char *key = NULL;
			if(mt_yaml_dup_scalar(p, &key) < 0)
				return -1;
			if(mt_yaml_next(p) < 0) {
				free(key);
				return -1;
			}
			if(strcmp(key, "from") == 0) {
				if(mt_parse_string_seq(p, &bind.from, &bind.num_from) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "to") == 0) {
				if(mt_yaml_dup_scalar(p, &bind.to) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "value") == 0) {
				if(mt_yaml_dup_scalar(p, &bind.value) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "case") == 0) {
				if(mt_parse_bind_cases(p, &bind) < 0) {
					free(key);
					return -1;
				}
			} else {
				mt_yaml_warn_unknown(p, "bind key", key);
				free(key);
				if(mt_yaml_skip(p) < 0)
					return -1;
				continue;
			}
			free(key);
		}
		if(!bind.to || (bind.value && bind.num_cases > 0) ||
		   ((!bind.value && bind.num_cases == 0) && bind.num_from == 0) ||
		   (bind.num_cases > 0 && bind.num_from == 0)) {
			mt_free_binds(&bind, 1);
			return mt_yaml_error(p, "bind requires to and either value, from, or case with from");
		}
		tmp = realloc(*binds, sizeof(**binds) * (*count + 1));
		if(!tmp) {
			perror("realloc");
			mt_free_binds(&bind, 1);
			return -1;
		}
		*binds = tmp;
		(*binds)[*count] = bind;
		(*count)++;
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_parse_deps(struct mt_yaml_parser *p, struct mt_dep **deps, int *count)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected dependencies sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		struct mt_dep dep;
		struct mt_dep *tmp;
		memset(&dep, 0, sizeof dep);
		if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected dependency mapping") < 0)
			return -1;
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			char *key = NULL;
			if(mt_yaml_dup_scalar(p, &key) < 0)
				return -1;
			if(mt_yaml_next(p) < 0) {
				free(key);
				return -1;
			}
			if(strcmp(key, "name") == 0) {
				if(mt_yaml_dup_scalar(p, &dep.name) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "binds") == 0) {
				if(mt_parse_binds(p, &dep.binds, &dep.num_binds) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "require_if") == 0) {
				if(mt_parse_conditions(p, &dep.require_if, &dep.num_require_if) < 0) {
					free(key);
					return -1;
				}
			} else {
				mt_yaml_warn_unknown(p, "dependency key", key);
				free(key);
				if(mt_yaml_skip(p) < 0)
					return -1;
				continue;
			}
			free(key);
		}
		if(!dep.name) {
			mt_free_deps(&dep, 1);
			return mt_yaml_error(p, "dependency requires name");
		}
		tmp = realloc(*deps, sizeof(**deps) * (*count + 1));
		if(!tmp) {
			perror("realloc");
			mt_free_deps(&dep, 1);
			return -1;
		}
		*deps = tmp;
		(*deps)[*count] = dep;
		(*count)++;
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_parse_components(struct mt_yaml_parser *p, struct mt_component **components, int *count)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected components sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		struct mt_component component;
		struct mt_component *tmp;
		memset(&component, 0, sizeof component);
		if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected component mapping") < 0)
			return -1;
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			char *key = NULL;
			if(mt_yaml_dup_scalar(p, &key) < 0)
				return -1;
			if(mt_yaml_next(p) < 0) {
				free(key);
				return -1;
			}
			if(strcmp(key, "name") == 0) {
				if(mt_yaml_dup_scalar(p, &component.name) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "tupfile") == 0) {
				if(mt_yaml_dup_scalar(p, &component.tupfile) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "function") == 0) {
				if(mt_yaml_dup_scalar(p, &component.function) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "binds") == 0) {
				if(mt_parse_binds(p, &component.binds, &component.num_binds) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "dependencies") == 0) {
				if(mt_parse_deps(p, &component.deps, &component.num_deps) < 0) {
					free(key);
					return -1;
				}
			} else {
				mt_yaml_warn_unknown(p, "component key", key);
				free(key);
				if(mt_yaml_skip(p) < 0)
					return -1;
				continue;
			}
			free(key);
		}
		if(!component.name) {
			free(component.tupfile);
			free(component.function);
			mt_free_binds(component.binds, component.num_binds);
			mt_free_deps(component.deps, component.num_deps);
			return mt_yaml_error(p, "component requires name");
		}
		if((component.tupfile && !component.function) || (!component.tupfile && component.function)) {
			free(component.name);
			free(component.tupfile);
			free(component.function);
			mt_free_binds(component.binds, component.num_binds);
			mt_free_deps(component.deps, component.num_deps);
			return mt_yaml_error(p, "component requires tupfile and function together");
		}
		tmp = realloc(*components, sizeof(**components) * (*count + 1));
		if(!tmp) {
			perror("realloc");
			free(component.name);
			free(component.tupfile);
			free(component.function);
			mt_free_binds(component.binds, component.num_binds);
			mt_free_deps(component.deps, component.num_deps);
			return -1;
		}
		*components = tmp;
		(*components)[*count] = component;
		(*count)++;
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_parse_profiles(struct mt_yaml_parser *p, struct mt_profile **profiles, int *count)
{
	if(mt_yaml_expect(p, YAML_SEQUENCE_START_EVENT, "expected profiles sequence") < 0)
		return -1;
	if(mt_yaml_next(p) < 0)
		return -1;
	while(p->event.type != YAML_SEQUENCE_END_EVENT) {
		struct mt_profile profile;
		struct mt_profile *tmp;
		memset(&profile, 0, sizeof profile);
		if(mt_yaml_expect(p, YAML_MAPPING_START_EVENT, "expected profile mapping") < 0)
			return -1;
		if(mt_yaml_next(p) < 0)
			return -1;
		while(p->event.type != YAML_MAPPING_END_EVENT) {
			char *key = NULL;
			if(mt_yaml_dup_scalar(p, &key) < 0)
				return -1;
			if(mt_yaml_next(p) < 0) {
				free(key);
				return -1;
			}
			if(strcmp(key, "name") == 0) {
				if(mt_yaml_dup_scalar(p, &profile.name) < 0) {
					free(key);
					return -1;
				}
				if(mt_yaml_next(p) < 0) {
					free(key);
					return -1;
				}
			} else if(strcmp(key, "set") == 0) {
				if(mt_parse_string_map(p, &profile.set, &profile.num_set) < 0) {
					free(key);
					return -1;
				}
			} else {
				mt_yaml_warn_unknown(p, "profile key", key);
				free(key);
				if(mt_yaml_skip(p) < 0)
					return -1;
				continue;
			}
			free(key);
		}
		if(!profile.name) {
			mt_cleanup_profile(&profile);
			return mt_yaml_error(p, "profile requires name");
		}
		tmp = realloc(*profiles, sizeof(**profiles) * (*count + 1));
		if(!tmp) {
			perror("realloc");
			mt_cleanup_profile(&profile);
			return -1;
		}
		*profiles = tmp;
		(*profiles)[*count] = profile;
		(*count)++;
		if(mt_yaml_next(p) < 0)
			return -1;
	}
	return mt_yaml_next(p);
}

static int mt_load_file(const char *filename, struct mt_file *mf, char **err)
{
	FILE *f;
	long len;
	char *buf = NULL;
	struct mt_yaml_parser p;
	char *key = NULL;
	int rc = -1;

	memset(mf, 0, sizeof *mf);
	memset(&p, 0, sizeof p);
	*err = NULL;

	f = fopen(filename, "r");
	if(!f) {
		size_t msglen = strlen(filename) + 64;
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
		goto out;
	}
	buf = malloc(len + 1);
	if(!buf) {
		perror("malloc");
		goto out;
	}
	if(len > 0 && fread(buf, 1, len, f) != (size_t)len) {
		perror(filename);
		goto out;
	}
	buf[len] = 0;

	if(!yaml_parser_initialize(&p.parser)) {
		*err = strdup("unable to initialize yaml parser");
		if(!*err)
			perror("strdup");
		goto out;
	}
	p.filename = filename;
	p.err = err;
	yaml_parser_set_input_string(&p.parser, (const unsigned char*)buf, len);

	if(mt_yaml_next(&p) < 0)
		goto out_yaml;
	if(mt_yaml_expect(&p, YAML_STREAM_START_EVENT, "expected stream start") < 0)
		goto out_yaml;
	if(mt_yaml_next(&p) < 0)
		goto out_yaml;
	if(mt_yaml_expect(&p, YAML_DOCUMENT_START_EVENT, "expected document start") < 0)
		goto out_yaml;
	if(mt_yaml_next(&p) < 0)
		goto out_yaml;
	if(mt_yaml_expect(&p, YAML_MAPPING_START_EVENT, "expected top-level mapping") < 0)
		goto out_yaml;
	if(mt_yaml_next(&p) < 0)
		goto out_yaml;
	while(p.event.type != YAML_MAPPING_END_EVENT) {
		if(mt_yaml_dup_scalar(&p, &key) < 0)
			goto out_yaml;
		if(mt_yaml_next(&p) < 0)
			goto out_yaml;
		if(strcmp(key, "defaults") == 0) {
			if(mt_yaml_expect(&p, YAML_MAPPING_START_EVENT, "expected defaults mapping") < 0)
				goto out_yaml;
			if(mt_yaml_next(&p) < 0)
				goto out_yaml;
			while(p.event.type != YAML_MAPPING_END_EVENT) {
				char *k = NULL;
				char *v = NULL;
				if(mt_yaml_dup_scalar(&p, &k) < 0)
					goto out_yaml;
				if(mt_yaml_next(&p) < 0) {
					free(k);
					goto out_yaml;
				}
				if(mt_yaml_dup_scalar(&p, &v) < 0) {
					free(k);
					goto out_yaml;
				}
				if(mt_add_kv_raw(&mf->defaults, &mf->num_defaults, k, v) < 0) {
					free(k);
					free(v);
					goto out_yaml;
				}
				if(mt_yaml_next(&p) < 0)
					goto out_yaml;
			}
			if(mt_yaml_next(&p) < 0)
				goto out_yaml;
		} else if(strcmp(key, "auto_compiledb") == 0) {
			char *value = NULL;
			if(mt_yaml_dup_scalar(&p, &value) < 0)
				goto out_yaml;
			if(strcmp(value, "true") == 0) {
				mf->auto_compiledb = 1;
			} else if(strcmp(value, "false") == 0) {
				mf->auto_compiledb = 0;
			} else {
				free(value);
				mt_yaml_error(&p, "auto_compiledb must be true or false");
				goto out_yaml;
			}
			mf->auto_compiledb_set = 1;
			free(value);
			if(mt_yaml_next(&p) < 0)
				goto out_yaml;
		} else if(strcmp(key, "profiles") == 0) {
			if(mt_parse_profiles(&p, &mf->profiles, &mf->num_profiles) < 0)
				goto out_yaml;
		} else if(strcmp(key, "components") == 0) {
			if(mt_parse_components(&p, &mf->components, &mf->num_components) < 0)
				goto out_yaml;
		} else {
			mt_yaml_warn_unknown(&p, "top-level key", key);
			free(key);
			key = NULL;
			if(mt_yaml_skip(&p) < 0)
				goto out_yaml;
			continue;
		}
		free(key);
		key = NULL;
	}
	rc = 0;
out_yaml:
	free(key);
	yaml_event_delete(&p.event);
	yaml_parser_delete(&p.parser);
	if(rc < 0)
		mt_free_file(mf);
out:
	free(buf);
	fclose(f);
	return rc;
}

static char *mt_join(const char *a, const char *b)
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

static char *mt_relpath(const char *from_dir, const char *to_path)
{
	char *from = strdup(from_dir);
	char *to = strdup(to_path);
	char *fromparts[128];
	char *toparts[128];
	char *save = NULL;
	char *tok;
	int fromn = 0;
	int ton = 0;
	int i = 0;
	size_t len = 0;
	char *out;
	int first = 1;

	if(!from || !to) {
		perror("strdup");
		free(from);
		free(to);
		return NULL;
	}
	for(tok = strtok_r(from, "/", &save); tok; tok = strtok_r(NULL, "/", &save))
		fromparts[fromn++] = tok;
	save = NULL;
	for(tok = strtok_r(to, "/", &save); tok; tok = strtok_r(NULL, "/", &save))
		toparts[ton++] = tok;
	while(i < fromn && i < ton && strcmp(fromparts[i], toparts[i]) == 0)
		i++;
	if(i == fromn && i == ton) {
		free(from);
		free(to);
		return strdup(".");
	}
	{
		int common = i;
		for(i = common; i < fromn; i++)
			len += 3;
		for(i = common; i < ton; i++)
			len += strlen(toparts[i]) + 1;
		out = malloc(len + 1);
		if(!out) {
			perror("malloc");
			free(from);
			free(to);
			return NULL;
		}
		out[0] = 0;
		for(i = common; i < fromn; i++) {
			strcat(out, first ? ".." : "/..");
			first = 0;
		}
		for(i = common; i < ton; i++) {
			if(!first)
				strcat(out, "/");
			strcat(out, toparts[i]);
			first = 0;
		}
	}
	free(from);
	free(to);
	return out;
}

static struct mt_component *mt_find_component(struct mt_file *mf, const char *name)
{
	int x;
	for(x=0; x<mf->num_components; x++) {
		if(strcmp(mf->components[x].name, name) == 0)
			return &mf->components[x];
	}
	return NULL;
}

static struct mt_profile *mt_find_profile(struct mt_file *mf, const char *name)
{
	int x;
	for(x=0; x<mf->num_profiles; x++) {
		if(strcmp(mf->profiles[x].name, name) == 0)
			return &mf->profiles[x];
	}
	return NULL;
}

static int mt_parse_label(const char *text, const char *current_package, struct mt_label *label, char **err)
{
	const char *colon;
	const char *pkgstart;
	memset(label, 0, sizeof *label);
	pkgstart = text;
	if(text[0] == '@') {
		const char *slashes = strstr(text, "//");
		if(!slashes || slashes == text + 1)
			goto bad;
		label->repo = strndup(text + 1, slashes - (text + 1));
		if(!label->repo) {
			perror("strndup");
			return -1;
		}
		pkgstart = slashes;
	}
	if(strncmp(pkgstart, "//", 2) == 0) {
		colon = strchr(pkgstart + 2, ':');
		if(!colon || colon[1] == 0)
			goto bad;
		label->package = strndup(pkgstart + 2, colon - (pkgstart + 2));
		label->component = strdup(colon + 1);
	} else if(pkgstart[0] == ':') {
		if(!current_package || pkgstart[1] == 0)
			goto bad;
		label->package = strdup(current_package);
		label->component = strdup(pkgstart + 1);
	} else {
		label->package = strdup(current_package ? current_package : "");
		label->component = strdup(pkgstart);
	}
	if(!label->package || !label->component) {
		perror("strdup");
		free(label->repo);
		free(label->package);
		free(label->component);
		return -1;
	}
	return 0;
bad:
	free(label->repo);
	*err = strdup("invalid MetaTup label");
	if(!*err)
		perror("strdup");
	return -1;
}

static void mt_free_label(struct mt_label *label)
{
	free(label->repo);
	free(label->package);
	free(label->component);
}

static int mt_load_labeled_component(struct mt_state *st, const char *label_text,
				     const char *current_repo_root_abs, const char *current_repo_root_rel,
				     const char *current_package, struct mt_file *mf,
				     struct mt_component **component, char **repo_root_abs, char **repo_root_rel,
				     char **package, char **err)
{
	struct mt_label label;
	char *meta = NULL;
	char *target_repo_root_abs = NULL;
	char *target_repo_root_rel = NULL;
	int rc = -1;
	(void)st;
	if(mt_parse_label(label_text, current_package, &label, err) < 0)
		return -1;
	if(label.repo) {
		char *repo_path = NULL;
		char *repo_err = NULL;
		char *repo_rel = NULL;
		if(metatup_repo_materialize(current_repo_root_abs, label.repo, &repo_path, &repo_err) < 0) {
			*err = repo_err ? repo_err : strdup("unable to materialize repository");
			mt_free_label(&label);
			return -1;
		}
		target_repo_root_abs = repo_path;
		repo_rel = mt_relpath(st->root, target_repo_root_abs);
		if(repo_rel && strcmp(repo_rel, ".") == 0) {
			free(repo_rel);
			repo_rel = strdup("");
		}
		target_repo_root_rel = repo_rel;
		if(!target_repo_root_rel) {
			perror("malloc");
			goto out;
		}
	} else {
		target_repo_root_abs = strdup(current_repo_root_abs);
		target_repo_root_rel = strdup(current_repo_root_rel ? current_repo_root_rel : "");
		if(!target_repo_root_abs || !target_repo_root_rel) {
			perror("strdup");
			goto out;
		}
	}
	if(label.package[0] == 0) {
		meta = mt_join(target_repo_root_abs, "MetaTup.yaml");
	} else {
		char *pkg = mt_join(target_repo_root_abs, label.package);
		if(!pkg)
			goto out;
		meta = mt_join(pkg, "MetaTup.yaml");
		free(pkg);
	}
	if(!meta) {
		goto out;
	}
	if(mt_load_file(meta, mf, err) < 0)
		goto out;
	*component = mt_find_component(mf, label.component);
	if(!*component) {
		size_t len = strlen(label.component) + strlen(meta) + 64;
		*err = malloc(len);
		if(!*err) {
			perror("malloc");
			goto out;
		}
		snprintf(*err, len, "unable to find component '%s' in %s", label.component, meta);
		goto out;
	}
	*package = strdup(label.package);
	*repo_root_abs = strdup(target_repo_root_abs);
	*repo_root_rel = strdup(target_repo_root_rel);
	if(!*package || !*repo_root_abs || !*repo_root_rel) {
		perror("strdup");
		goto out;
	}
	rc = 0;
out:
	free(meta);
	free(target_repo_root_abs);
	free(target_repo_root_rel);
	mt_free_label(&label);
	if(rc < 0)
		mt_free_file(mf);
	return rc;
}

static int mt_eval_expr(const char *expr, struct mt_kv *args, int num_args,
			const char *current_repo_root_abs, const char *current_repo_root_rel,
			const char *current_package, const char *root_name, char **out)
{
	size_t cap = strlen(expr) + 16;
	size_t len = 0;
	char *buf = malloc(cap);
	const char *p = expr;
	if(!buf) {
		perror("malloc");
		return -1;
	}
	while(*p) {
		if(p[0] == '$' && p[1] == '(') {
			const char *body = p + 2;
			const char *end = strchr(body, ')');
			char name[256];
			const char *val;
			char *owned_val = NULL;
			size_t nlen;
			if(!end) {
				free(buf);
				fprintf(stderr, "tup error: Unterminated bind expression '%s'.\n", expr);
				return -1;
			}
			nlen = end - body;
			if(nlen >= sizeof(name)) {
				free(buf);
				fprintf(stderr, "tup error: Bind expression too long '%s'.\n", expr);
				return -1;
			}
			memcpy(name, body, nlen);
			name[nlen] = 0;
			if(strncmp(name, "from", 4) == 0 && isspace((unsigned char)name[4])) {
				struct mt_label label;
				const char *s = name + 4;
				char *label_text = NULL;
				char *ret_name = NULL;
				char *dep_build = NULL;
				char *rewritten = NULL;
				size_t rewritten_len;

				memset(&label, 0, sizeof label);
				while(isspace((unsigned char)*s))
					s++;
				if(mt_parse_quoted_string(&s, &label_text) < 0) {
					free(buf);
					return -1;
				}
				while(isspace((unsigned char)*s))
					s++;
				if(mt_parse_quoted_string(&s, &ret_name) < 0) {
					free(label_text);
					free(buf);
					return -1;
				}
				while(isspace((unsigned char)*s))
					s++;
				if(*s != 0) {
					fprintf(stderr, "tup error: Invalid bind expression '%s'.\n", expr);
					free(label_text);
					free(ret_name);
					free(buf);
					return -1;
				}
				{
					char *label_err = NULL;
					if(mt_parse_label(label_text, current_package, &label, &label_err) < 0) {
						if(label_err)
							fprintf(stderr, "%s\n", label_err);
						free(label_err);
						free(label_text);
						free(ret_name);
						free(buf);
						return -1;
					}
				}
				{
					const char *dep_repo_root_rel = current_repo_root_rel;
					char *repo_path = NULL;
					char *repo_err = NULL;
					char *owned_repo_root_rel = NULL;
					if(label.repo) {
						if(metatup_repo_materialize(current_repo_root_abs, label.repo, &repo_path, &repo_err) < 0) {
							if(repo_err)
								fprintf(stderr, "%s\n", repo_err);
							free(repo_err);
							mt_free_label(&label);
							free(label_text);
							free(ret_name);
							free(buf);
							return -1;
						}
						owned_repo_root_rel = mt_relpath(current_repo_root_abs, repo_path);
						if(owned_repo_root_rel && strcmp(owned_repo_root_rel, ".") == 0) {
							free(owned_repo_root_rel);
							owned_repo_root_rel = strdup("");
						}
						free(repo_path);
						if(!owned_repo_root_rel) {
							perror("malloc");
							mt_free_label(&label);
							free(label_text);
							free(ret_name);
							free(buf);
							return -1;
						}
						dep_repo_root_rel = owned_repo_root_rel;
					}
					if(mt_make_dep_build_name(root_name, dep_repo_root_rel, label.package, label.component, &dep_build) < 0) {
						free(owned_repo_root_rel);
						mt_free_label(&label);
						free(label_text);
						free(ret_name);
						free(buf);
						return -1;
					}
					free(owned_repo_root_rel);
				}
				rewritten_len = strlen(dep_build) + strlen(ret_name) + 16;
				rewritten = malloc(rewritten_len);
				if(!rewritten) {
					perror("malloc");
					free(dep_build);
					mt_free_label(&label);
					free(label_text);
					free(ret_name);
					free(buf);
					return -1;
				}
				snprintf(rewritten, rewritten_len, "$(from \"%s\" \"%s\")", dep_build, ret_name);
				val = rewritten;
				owned_val = rewritten;
				free(dep_build);
				mt_free_label(&label);
				free(label_text);
				free(ret_name);
			} else {
				val = mt_get_kv(args, num_args, name);
				if(!val)
					val = "";
			}
			if(len + strlen(val) + 1 > cap) {
				cap = len + strlen(val) + 16;
				buf = realloc(buf, cap);
				if(!buf) {
					perror("realloc");
					free(owned_val);
					return -1;
				}
			}
			memcpy(buf + len, val, strlen(val));
			len += strlen(val);
			free(owned_val);
			p = end + 1;
		} else {
			if(len + 2 > cap) {
				cap *= 2;
				buf = realloc(buf, cap);
				if(!buf) {
					perror("realloc");
					return -1;
				}
			}
			buf[len++] = *p++;
		}
	}
	buf[len] = 0;
	*out = buf;
	return 0;
}

static int mt_apply_binds(struct mt_bind *binds, int num_binds, struct mt_kv *src, int num_src,
			  const char *current_repo_root_abs, const char *current_repo_root_rel,
			  const char *current_package, const char *root_name,
			  struct mt_kv **dst, int *num_dst)
{
	struct mt_kv *scope = NULL;
	int num_scope = 0;
	int x;
	if(mt_clone_kvs(src, num_src, &scope, &num_scope) < 0)
		return -1;
	for(x=0; x<num_binds; x++) {
		char *value = NULL;
		const char *srcval;
		if(binds[x].num_cases > 0 || binds[x].default_case) {
			srcval = mt_get_kv(scope, num_scope, binds[x].from[0]);
			if(srcval) {
				int y;
				for(y=0; y<binds[x].num_cases; y++) {
					if(strcmp(srcval, binds[x].cases[y].key) == 0) {
						if(mt_eval_expr(binds[x].cases[y].value, scope, num_scope,
								current_repo_root_abs, current_repo_root_rel,
								current_package, root_name, &value) < 0)
							goto err;
						break;
					}
				}
			}
			if(!value) {
				if(mt_eval_expr(binds[x].default_case ? binds[x].default_case : "", scope, num_scope,
						current_repo_root_abs, current_repo_root_rel,
						current_package, root_name, &value) < 0)
					goto err;
			}
		} else if(binds[x].value) {
			if(mt_eval_expr(binds[x].value, scope, num_scope, current_repo_root_abs, current_repo_root_rel,
					current_package, root_name, &value) < 0)
				goto err;
		} else {
			srcval = mt_get_kv(scope, num_scope, binds[x].from[0]);
			value = strdup(srcval ? srcval : "");
			if(!value) {
				perror("strdup");
				goto err;
			}
		}
		if(mt_set_kv(dst, num_dst, binds[x].to, value) < 0) {
			free(value);
			goto err;
		}
		if(mt_set_kv(&scope, &num_scope, binds[x].to, value) < 0) {
			free(value);
			goto err;
		}
		free(value);
	}
	mt_free_kvs(scope, num_scope);
	return 0;
err:
	mt_free_kvs(scope, num_scope);
	return -1;
}

static int mt_is_truthy(const char *value)
{
	if(!value || value[0] == 0)
		return 0;
	return strcmp(value, "0") != 0 &&
	       strcmp(value, "false") != 0 &&
	       strcmp(value, "False") != 0 &&
	       strcmp(value, "n") != 0 &&
	       strcmp(value, "no") != 0;
}

static int mt_eval_condition(struct mt_condition *cond, struct mt_kv *args, int num_args)
{
	int x;
	switch(cond->type) {
	case MT_COND_PROFILE_ENABLED:
		return mt_is_truthy(mt_get_kv(args, num_args, cond->profile));
	case MT_COND_ARGS_EQUAL:
		for(x=0; x<cond->num_args_equal; x++) {
			const char *value = mt_get_kv(args, num_args, cond->args_equal[x].key);
			if(!value || strcmp(value, cond->args_equal[x].value) != 0)
				return 0;
		}
		return 1;
	case MT_COND_AND:
		for(x=0; x<cond->num_children; x++) {
			if(!mt_eval_condition(&cond->children[x], args, num_args))
				return 0;
		}
		return 1;
	case MT_COND_OR:
		for(x=0; x<cond->num_children; x++) {
			if(mt_eval_condition(&cond->children[x], args, num_args))
				return 1;
		}
		return 0;
	case MT_COND_NOT:
		return cond->num_children == 1 ? !mt_eval_condition(&cond->children[0], args, num_args) : 0;
	}
	return 0;
}

static int mt_dep_enabled(struct mt_dep *dep, struct mt_kv *args, int num_args)
{
	int x;
	if(dep->num_require_if == 0)
		return 1;
	for(x=0; x<dep->num_require_if; x++) {
		if(mt_eval_condition(&dep->require_if[x], args, num_args))
			return 1;
	}
	return 0;
}

static char *mt_sanitize(const char *name)
{
	char *out = strdup(name);
	char *p;
	if(!out) {
		perror("strdup");
		return NULL;
	}
	for(p = out; *p; p++) {
		if(!isalnum((unsigned char)*p))
			*p = '_';
	}
	return out;
}

static int mt_make_dep_build_name(const char *root_build_name, const char *repo_root_rel,
				  const char *package, const char *component, char **out)
{
	char *location = NULL;
	char *sp;
	char *sc = mt_sanitize(component);
	size_t len;
	if(repo_root_rel && repo_root_rel[0] && package && package[0]) {
		location = mt_join(repo_root_rel, package);
	} else if(repo_root_rel && repo_root_rel[0]) {
		location = strdup(repo_root_rel);
	} else if(package && package[0]) {
		location = strdup(package);
	} else {
		location = strdup("root");
	}
	sp = location ? mt_sanitize(location) : NULL;
	free(location);
	if(!sp || !sc) {
		free(sp);
		free(sc);
		return -1;
	}
	len = strlen(root_build_name) + strlen(sp) + strlen(sc) + 5;
	*out = malloc(len);
	if(!*out) {
		perror("malloc");
		free(sp);
		free(sc);
		return -1;
	}
	snprintf(*out, len, "%s__%s_%s", root_build_name, sp, sc);
	free(sp);
	free(sc);
	return 0;
}

static int mt_existing_builddir(struct mt_state *st, const char *name, char **out)
{
	int x;
	if(!st->have_existing_tb)
		return 0;
	for(x=0; x<st->existing_tb.num_builds; x++) {
		if(strcmp(st->existing_tb.builds[x].name, name) == 0) {
			*out = strdup(st->existing_tb.builds[x].builddir);
			if(!*out) {
				perror("strdup");
				return -1;
			}
			return 1;
		}
	}
	return 0;
}

static int mt_add_generated(struct mt_state *st, struct mt_generated_build *b)
{
	struct mt_generated_build *tmp = realloc(st->generated, sizeof(*tmp) * (st->num_generated + 1));
	if(!tmp) {
		perror("realloc");
		return -1;
	}
	st->generated = tmp;
	st->generated[st->num_generated] = *b;
	memset(b, 0, sizeof *b);
	st->num_generated++;
	return 0;
}

static int mt_find_generated(struct mt_state *st, const char *identity)
{
	int x;
	for(x=0; x<st->num_generated; x++) {
		if(strcmp(st->generated[x].identity, identity) == 0)
			return x;
	}
	return -1;
}

static int mt_append_dep(struct mt_generated_build *b, const char *build, const char *as)
{
	struct tupbuild_dep *tmp = realloc(b->depends, sizeof(*tmp) * (b->num_depends + 1));
	if(!tmp) {
		perror("realloc");
		return -1;
	}
	b->depends = tmp;
	b->depends[b->num_depends].build = strdup(build);
	b->depends[b->num_depends].as = strdup(as);
	if(!b->depends[b->num_depends].build || !b->depends[b->num_depends].as) {
		perror("strdup");
		return -1;
	}
	b->num_depends++;
	return 0;
}

static int mt_append_dep_result(struct mt_dep_result *dr, struct mt_generated_build *build)
{
	struct mt_generated_build **tmp = realloc(dr->builds, sizeof(*tmp) * (dr->num_builds + 1));
	if(!tmp) {
		perror("realloc");
		return -1;
	}
	dr->builds = tmp;
	dr->builds[dr->num_builds] = build;
	dr->num_builds++;
	return 0;
}

static int mt_collect_component_builds(struct mt_state *st,
				       const char *repo_root_abs, const char *repo_root_rel,
				       const char *package, struct mt_component *component,
				       const char *concrete_name, const char *concrete_builddir, const char *root_name,
				       struct mt_kv *incoming_args, int num_incoming_args, struct mt_dep_result *result, char **err);

static int mt_expand_component_deps(struct mt_state *st, struct mt_generated_build *parent,
				    const char *repo_root_abs, const char *repo_root_rel,
				    const char *package, struct mt_component *component,
				    const char *root_name, struct mt_kv *effective_args, int num_effective_args,
				    char **err)
{
	int x;
	for(x=0; x<component->num_deps; x++) {
		struct mt_file dep_file;
		struct mt_component *dep_component = NULL;
		struct mt_kv *dep_args = NULL;
		int num_dep_args = 0;
		char *dep_package = NULL;
		struct mt_dep_result dep_result;
		const char *alias;
		int y;

		memset(&dep_file, 0, sizeof dep_file);
		memset(&dep_result, 0, sizeof dep_result);
		if(!mt_dep_enabled(&component->deps[x], effective_args, num_effective_args))
			continue;
		if(mt_clone_non_from_kvs(effective_args, num_effective_args, &dep_args, &num_dep_args) < 0)
			return -1;
		if(component->deps[x].num_binds > 0) {
			struct mt_kv *bound_dep_args = NULL;
			int num_bound_dep_args = 0;
			if(mt_apply_binds(component->deps[x].binds, component->deps[x].num_binds,
					  effective_args, num_effective_args,
					  repo_root_abs, repo_root_rel, package, root_name,
					  &bound_dep_args, &num_bound_dep_args) < 0) {
				mt_free_kvs(dep_args, num_dep_args);
				return -1;
			}
			mt_free_kvs(dep_args, num_dep_args);
			dep_args = bound_dep_args;
			num_dep_args = num_bound_dep_args;
		}
		{
			char *dep_repo_root_abs = NULL;
			char *dep_repo_root_rel = NULL;
			if(mt_load_labeled_component(st, component->deps[x].name, repo_root_abs, repo_root_rel, package,
						     &dep_file, &dep_component,
						     &dep_repo_root_abs, &dep_repo_root_rel, &dep_package, err) < 0) {
				mt_free_kvs(dep_args, num_dep_args);
				return -1;
			}
			if(mt_collect_component_builds(st, dep_repo_root_abs, dep_repo_root_rel, dep_package,
						      dep_component, NULL, NULL, root_name,
						      dep_args, num_dep_args, &dep_result, err) < 0) {
				mt_free_file(&dep_file);
				mt_free_kvs(dep_args, num_dep_args);
				free(dep_repo_root_abs);
				free(dep_repo_root_rel);
				free(dep_package);
				return -1;
			}
			free(dep_repo_root_abs);
			free(dep_repo_root_rel);
		}
		alias = strrchr(component->deps[x].name, ':');
		alias = alias ? alias + 1 : component->deps[x].name;
		for(y=0; y<dep_result.num_builds; y++) {
			if(mt_append_dep(parent, dep_result.builds[y]->name, alias) < 0) {
				mt_free_dep_result(&dep_result);
				mt_free_file(&dep_file);
				mt_free_kvs(dep_args, num_dep_args);
				free(dep_package);
				return -1;
			}
		}
		mt_free_dep_result(&dep_result);
		mt_free_file(&dep_file);
		mt_free_kvs(dep_args, num_dep_args);
		free(dep_package);
	}
	return 0;
}

static int mt_build_component(struct mt_state *st,
			      const char *repo_root_abs, const char *repo_root_rel,
			      const char *package, struct mt_component *component,
			      const char *concrete_name, const char *concrete_builddir, const char *root_name,
			      struct mt_kv *incoming_args, int num_incoming_args, struct mt_generated_build **out, char **err)
{
	struct mt_generated_build build;
	struct mt_kv *effective_args = NULL;
	int num_effective_args = 0;
	int rc = -1;
	int enabled_deps = 0;
	int enabled_dep_idx = -1;
	int x;

	memset(&build, 0, sizeof build);
	if(mt_clone_kvs(incoming_args, num_incoming_args, &effective_args, &num_effective_args) < 0)
		goto out;
	if(component->num_binds > 0) {
		struct mt_kv *bound_args = NULL;
		int num_bound_args = 0;
		if(mt_apply_binds(component->binds, component->num_binds, effective_args, num_effective_args,
				  repo_root_abs, repo_root_rel, package, root_name, &bound_args, &num_bound_args) < 0)
			goto out;
		mt_free_kvs(effective_args, num_effective_args);
		effective_args = bound_args;
		num_effective_args = num_bound_args;
	}
	build.identity = strdup(concrete_name);
	build.name = strdup(concrete_name);
	if(!build.identity || !build.name) {
		perror("strdup");
		goto out;
	}
	{
		int idx = mt_find_generated(st, build.identity);
		if(idx >= 0) {
			*out = &st->generated[idx];
			rc = 0;
			goto out;
		}
	}
	if(!component->tupfile) {
		enabled_deps = 0;
		enabled_dep_idx = -1;
		for(x=0; x<component->num_deps; x++) {
			if(mt_dep_enabled(&component->deps[x], effective_args, num_effective_args)) {
				enabled_deps++;
				enabled_dep_idx = x;
			}
		}
		if(strcmp(concrete_name, root_name) != 0) {
			*err = strdup("intermediate dependency-only components should be expanded by the parent");
			if(!*err)
				perror("strdup");
			goto out;
		}
		if(enabled_deps == 1) {
			struct mt_file dep_file;
			struct mt_component *dep_component = NULL;
			struct mt_generated_build *dep_build = NULL;
			struct mt_kv *dep_args = NULL;
			int num_dep_args = 0;
			char *dep_package = NULL;
			memset(&dep_file, 0, sizeof dep_file);
			if(mt_clone_non_from_kvs(effective_args, num_effective_args, &dep_args, &num_dep_args) < 0)
				goto out;
			if(component->deps[enabled_dep_idx].num_binds > 0) {
				struct mt_kv *bound_dep_args = NULL;
				int num_bound_dep_args = 0;
				if(mt_apply_binds(component->deps[enabled_dep_idx].binds, component->deps[enabled_dep_idx].num_binds,
						  effective_args, num_effective_args,
						  repo_root_abs, repo_root_rel, package, root_name,
						  &bound_dep_args, &num_bound_dep_args) < 0) {
					mt_free_kvs(dep_args, num_dep_args);
					goto out;
				}
				mt_free_kvs(dep_args, num_dep_args);
				dep_args = bound_dep_args;
				num_dep_args = num_bound_dep_args;
			}
			{
				char *dep_repo_root_abs = NULL;
				char *dep_repo_root_rel = NULL;
				if(mt_load_labeled_component(st, component->deps[enabled_dep_idx].name,
							     repo_root_abs, repo_root_rel, package, &dep_file, &dep_component,
							     &dep_repo_root_abs, &dep_repo_root_rel, &dep_package, err) < 0) {
					mt_free_kvs(dep_args, num_dep_args);
					goto out;
				}
				if(mt_build_component(st, dep_repo_root_abs, dep_repo_root_rel, dep_package, dep_component,
						      concrete_name, concrete_builddir, root_name,
						      dep_args, num_dep_args, &dep_build, err) < 0) {
					mt_free_file(&dep_file);
					mt_free_kvs(dep_args, num_dep_args);
					free(dep_repo_root_abs);
					free(dep_repo_root_rel);
					free(dep_package);
					goto out;
				}
				free(dep_repo_root_abs);
				free(dep_repo_root_rel);
			}
			free(build.name);
			build.name = NULL;
			free(build.identity);
			build.identity = NULL;
			*out = dep_build;
			mt_free_file(&dep_file);
			mt_free_kvs(dep_args, num_dep_args);
			free(dep_package);
			rc = 0;
			goto out;
		}
		{
			struct mt_dep_result dep_result;
			memset(&dep_result, 0, sizeof dep_result);
			if(mt_collect_component_builds(st, repo_root_abs, repo_root_rel, package, component,
						      NULL, NULL, root_name,
						      effective_args, num_effective_args, &dep_result, err) < 0)
				goto out;
			if(dep_result.num_builds == 0) {
				mt_free_dep_result(&dep_result);
				*err = strdup("top-level dependency-only component did not resolve to any concrete builds");
				if(!*err)
					perror("strdup");
				goto out;
			}
			if(dep_result.num_builds == 1) {
				free(build.name);
				build.name = NULL;
				free(build.identity);
				build.identity = NULL;
				*out = dep_result.builds[0];
				mt_free_dep_result(&dep_result);
				rc = 0;
				goto out;
			}
			free(build.tupfile);
			build.tupfile = NULL;
			free(build.function);
			build.function = NULL;
			free(build.builddir);
			build.builddir = strdup(concrete_builddir);
			if(!build.builddir) {
				perror("strdup");
				mt_free_dep_result(&dep_result);
				goto out;
			}
			for(x=0; x<dep_result.num_builds; x++) {
				if(mt_append_dep(&build, dep_result.builds[x]->name, dep_result.builds[x]->name) < 0) {
					mt_free_dep_result(&dep_result);
					goto out;
				}
			}
			mt_free_dep_result(&dep_result);
			if(mt_add_generated(st, &build) < 0)
				goto out;
			*out = &st->generated[st->num_generated - 1];
			rc = 0;
			goto out;
		}
	}
	{
		char *pkg_dir = package && package[0] ? mt_join(repo_root_abs, package) : strdup(repo_root_abs);
		char *abs_tupfile = NULL;
		char *cwd_dir = strdup(st->cwd);
		if(!pkg_dir || !cwd_dir) {
			free(pkg_dir);
			free(cwd_dir);
			goto out;
		}
		abs_tupfile = mt_join(pkg_dir, component->tupfile);
		free(pkg_dir);
		if(!abs_tupfile) {
			free(cwd_dir);
			goto out;
		}
		build.tupfile = mt_relpath(cwd_dir, abs_tupfile);
		free(cwd_dir);
		free(abs_tupfile);
		if(!build.tupfile)
			goto out;
	}
	build.function = strdup(component->function);
	if(!build.function) {
		perror("strdup");
		goto out;
	}
	if(st->profile) {
		build.profile = strdup(st->profile);
		if(!build.profile) {
			perror("strdup");
			goto out;
		}
	}
	build.builddir = strdup(concrete_builddir);
	if(!build.builddir) {
		perror("strdup");
		goto out;
	}
	if(mt_clone_kvs_except(effective_args, num_effective_args, &build.args, &build.num_args, st->profile) < 0)
		goto out;
	if(mt_expand_component_deps(st, &build, repo_root_abs, repo_root_rel, package, component,
				    root_name, effective_args, num_effective_args, err) < 0)
		goto out;
	if(mt_add_generated(st, &build) < 0)
		goto out;
	*out = &st->generated[st->num_generated - 1];
	rc = 0;
out:
	mt_free_kvs(effective_args, num_effective_args);
	if(rc < 0)
		mt_free_generated_build(&build);
	return rc;
}

static int mt_collect_component_builds(struct mt_state *st,
				       const char *repo_root_abs, const char *repo_root_rel,
				       const char *package, struct mt_component *component,
				       const char *concrete_name, const char *concrete_builddir, const char *root_name,
				       struct mt_kv *incoming_args, int num_incoming_args, struct mt_dep_result *result, char **err)
{
	if(component->tupfile) {
		struct mt_generated_build *build = NULL;
		char *dep_name = NULL;
		char *dep_builddir = NULL;
		if(!concrete_name) {
			if(mt_make_dep_build_name(root_name, repo_root_rel, package, component->name, &dep_name) < 0)
				return -1;
			concrete_name = dep_name;
		}
		if(!concrete_builddir) {
			if(mt_existing_builddir(st, concrete_name, &dep_builddir) < 0) {
				free(dep_name);
				return -1;
			}
			if(!dep_builddir) {
				size_t len = strlen(concrete_name) + 9;
				dep_builddir = malloc(len);
				if(!dep_builddir) {
					perror("malloc");
					free(dep_name);
					return -1;
				}
				snprintf(dep_builddir, len, "./%s.build", concrete_name);
			}
			concrete_builddir = dep_builddir;
		}
		if(mt_build_component(st, repo_root_abs, repo_root_rel, package, component,
				      concrete_name, concrete_builddir, root_name,
				      incoming_args, num_incoming_args, &build, err) < 0) {
			free(dep_name);
			free(dep_builddir);
			return -1;
		}
		if(mt_append_dep_result(result, build) < 0) {
			free(dep_name);
			free(dep_builddir);
			return -1;
		}
		free(dep_name);
		free(dep_builddir);
		return 0;
	}

	{
		struct mt_kv *effective_args = NULL;
		int num_effective_args = 0;
		int x;
		int rc = -1;
		if(mt_clone_kvs(incoming_args, num_incoming_args, &effective_args, &num_effective_args) < 0)
			return -1;
		if(component->num_binds > 0) {
			struct mt_kv *bound_args = NULL;
			int num_bound_args = 0;
			if(mt_apply_binds(component->binds, component->num_binds, effective_args, num_effective_args,
					  repo_root_abs, repo_root_rel, package, root_name,
					  &bound_args, &num_bound_args) < 0) {
				mt_free_kvs(effective_args, num_effective_args);
				return -1;
			}
			mt_free_kvs(effective_args, num_effective_args);
			effective_args = bound_args;
			num_effective_args = num_bound_args;
		}
		for(x=0; x<component->num_deps; x++) {
			struct mt_file dep_file;
			struct mt_component *dep_component = NULL;
			struct mt_kv *dep_args = NULL;
			int num_dep_args = 0;
			char *dep_package = NULL;
			struct mt_dep_result nested;
			int y;
			memset(&dep_file, 0, sizeof dep_file);
			memset(&nested, 0, sizeof nested);
			if(!mt_dep_enabled(&component->deps[x], effective_args, num_effective_args))
				continue;
			if(mt_clone_non_from_kvs(effective_args, num_effective_args, &dep_args, &num_dep_args) < 0)
				goto out;
			if(component->deps[x].num_binds > 0) {
				struct mt_kv *bound_dep_args = NULL;
				int num_bound_dep_args = 0;
				if(mt_apply_binds(component->deps[x].binds, component->deps[x].num_binds,
						  effective_args, num_effective_args,
						  repo_root_abs, repo_root_rel, package, root_name,
						  &bound_dep_args, &num_bound_dep_args) < 0) {
					mt_free_kvs(dep_args, num_dep_args);
					goto out;
				}
				mt_free_kvs(dep_args, num_dep_args);
				dep_args = bound_dep_args;
				num_dep_args = num_bound_dep_args;
			}
			{
				char *dep_repo_root_abs = NULL;
				char *dep_repo_root_rel = NULL;
				if(mt_load_labeled_component(st, component->deps[x].name, repo_root_abs, repo_root_rel, package,
							     &dep_file, &dep_component,
							     &dep_repo_root_abs, &dep_repo_root_rel, &dep_package, err) < 0) {
					mt_free_kvs(dep_args, num_dep_args);
					goto out;
				}
				if(mt_collect_component_builds(st, dep_repo_root_abs, dep_repo_root_rel, dep_package,
							      dep_component, NULL, NULL, root_name,
							      dep_args, num_dep_args, &nested, err) < 0) {
					mt_free_file(&dep_file);
					mt_free_kvs(dep_args, num_dep_args);
					free(dep_repo_root_abs);
					free(dep_repo_root_rel);
					free(dep_package);
					goto out;
				}
				free(dep_repo_root_abs);
				free(dep_repo_root_rel);
			}
			for(y=0; y<nested.num_builds; y++) {
				if(mt_append_dep_result(result, nested.builds[y]) < 0) {
					mt_free_dep_result(&nested);
					mt_free_file(&dep_file);
					mt_free_kvs(dep_args, num_dep_args);
					free(dep_package);
					goto out;
				}
			}
			mt_free_dep_result(&nested);
			mt_free_file(&dep_file);
			mt_free_kvs(dep_args, num_dep_args);
			free(dep_package);
		}
		rc = 0;
out:
		mt_free_kvs(effective_args, num_effective_args);
		return rc;
	}
}

static int mt_read_existing_tupbuild(struct mt_state *st)
{
	FILE *f = fopen("TupBuild.yaml", "r");
	long len;
	char *buf;
	char *err = NULL;
	if(!f)
		return 0;
	if(fseek(f, 0, SEEK_END) < 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0) {
		perror("TupBuild.yaml");
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
		perror("TupBuild.yaml");
		free(buf);
		fclose(f);
		return -1;
	}
	buf[len] = 0;
	fclose(f);
	if(tupbuild_parse("TupBuild.yaml", buf, len, &st->existing_tb, &err) < 0) {
		fprintf(stderr, "%s\n", err ? err : "unable to parse TupBuild.yaml");
		free(err);
		free(buf);
		return -1;
	}
	free(buf);
	st->have_existing_tb = 1;
	st->strict = st->existing_tb.strict;
	return 0;
}

static int mt_find_generated_by_name(struct mt_state *st, const char *name)
{
	int x;
	for(x=0; x<st->num_generated; x++) {
		if(strcmp(st->generated[x].name, name) == 0)
			return x;
	}
	return -1;
}

static int mt_write_existing_build(FILE *f, struct tupbuild_build *b)
{
	int x;
	fprintf(f, "  - name: %s\n", b->name);
	fprintf(f, "    tupfile: %s\n", b->tupfile);
	fprintf(f, "    function: %s\n", b->function);
	fprintf(f, "    builddir: %s\n", b->builddir);
	if(b->profile)
		fprintf(f, "    profile: %s\n", b->profile);
	if(b->num_args > 0) {
		fprintf(f, "    args:\n");
		for(x=0; x<b->num_args; x++) {
			char *key = mt_yaml_quote(b->args[x].key);
			char *value = mt_yaml_quote(b->args[x].value);
			if(!key || !value) {
				free(key);
				free(value);
				return -1;
			}
			fprintf(f, "      \"%s\": \"%s\"\n", key, value);
			free(key);
			free(value);
		}
	}
	if(b->num_depends > 0) {
		fprintf(f, "    depends:\n");
		for(x=0; x<b->num_depends; x++) {
			fprintf(f, "      - build: %s\n", b->depends[x].build);
			fprintf(f, "        as: %s\n", b->depends[x].as);
		}
	}
	if(b->num_dists > 0) {
		fprintf(f, "    dists:\n");
		for(x=0; x<b->num_dists; x++) {
			fprintf(f, "      - from_return: %s\n", b->dists[x].from_return);
			fprintf(f, "        path: %s\n", b->dists[x].path);
		}
	}
	return 0;
}

static int mt_write_generated_build(FILE *f, struct mt_generated_build *b)
{
	int x;
	fprintf(f, "  - name: %s\n", b->name);
	if(b->tupfile)
		fprintf(f, "    tupfile: %s\n", b->tupfile);
	if(b->function)
		fprintf(f, "    function: %s\n", b->function);
	fprintf(f, "    builddir: %s\n", b->builddir);
	if(b->profile)
		fprintf(f, "    profile: %s\n", b->profile);
	if(b->num_args > 0) {
		fprintf(f, "    args:\n");
		for(x=0; x<b->num_args; x++) {
			char *key = mt_yaml_quote(b->args[x].key);
			char *value = mt_yaml_quote(b->args[x].value);
			if(!key || !value) {
				free(key);
				free(value);
				return -1;
			}
			fprintf(f, "      \"%s\": \"%s\"\n", key, value);
			free(key);
			free(value);
		}
	}
	if(b->num_depends > 0) {
		fprintf(f, "    depends:\n");
		for(x=0; x<b->num_depends; x++) {
			fprintf(f, "      - build: %s\n", b->depends[x].build);
			fprintf(f, "        as: %s\n", b->depends[x].as);
		}
	}
	if(b->num_dists > 0) {
		fprintf(f, "    dists:\n");
		for(x=0; x<b->num_dists; x++) {
			fprintf(f, "      - from_return: %s\n", b->dists[x].from_return);
			fprintf(f, "        path: %s\n", b->dists[x].path);
		}
	}
	return 0;
}

static int mt_write_tupbuild(struct mt_state *st)
{
	FILE *f = fopen("TupBuild.yaml", "w");
	int *emitted = NULL;
	int x;
	if(!f) {
		perror("TupBuild.yaml");
		return -1;
	}
	emitted = calloc(st->num_generated, sizeof *emitted);
	if(st->num_generated > 0 && !emitted) {
		perror("calloc");
		fclose(f);
		return -1;
	}
	fprintf(f, "strict: %s\n", st->strict ? "true" : "false");
	fprintf(f, "auto_compiledb: %s\n", st->auto_compiledb ? "true" : "false");
	fprintf(f, "builds:\n");
	if(st->have_existing_tb) {
		for(x=0; x<st->existing_tb.num_builds; x++) {
			int gidx = mt_find_generated_by_name(st, st->existing_tb.builds[x].name);
			if(gidx >= 0) {
				mt_write_generated_build(f, &st->generated[gidx]);
				emitted[gidx] = 1;
			} else {
				mt_write_existing_build(f, &st->existing_tb.builds[x]);
			}
		}
	}
	for(x=0; x<st->num_generated; x++) {
		if(!emitted[x])
			mt_write_generated_build(f, &st->generated[x]);
	}
	if(fclose(f) != 0) {
		perror("fclose");
		free(emitted);
		return -1;
	}
	free(emitted);
	return 0;
}

static int mt_add_cli_dist(struct mt_state *st, const char *spec)
{
	struct tupbuild_dist *tmp = realloc(st->cli_dists, sizeof(*tmp) * (st->num_cli_dists + 1));
	const char *eq;
	if(!tmp) {
		perror("realloc");
		return -1;
	}
	st->cli_dists = tmp;
	eq = strchr(spec, '=');
	if(eq) {
		st->cli_dists[st->num_cli_dists].from_return = strndup(spec, eq - spec);
		st->cli_dists[st->num_cli_dists].path = strdup(eq + 1);
	} else {
		st->cli_dists[st->num_cli_dists].from_return = strdup(spec);
		st->cli_dists[st->num_cli_dists].path = NULL;
	}
	if(!st->cli_dists[st->num_cli_dists].from_return || (eq && !st->cli_dists[st->num_cli_dists].path)) {
		perror("strdup");
		return -1;
	}
	st->num_cli_dists++;
	return 0;
}

int metatup_gen(int argc, char **argv)
{
	struct mt_state st;
	struct mt_file root_file;
	struct mt_component *root_component = NULL;
	struct mt_generated_build *root_build = NULL;
	struct mt_kv *root_args = NULL;
	int num_root_args = 0;
	char *root_build_name = NULL;
	char *root_builddir = NULL;
	char *err = NULL;
	int x;

	memset(&st, 0, sizeof st);
	memset(&root_file, 0, sizeof root_file);
	st.strict = 1;

	if(argc < 1) {
		fprintf(stderr, "Usage: tup gen <component> [name] [-P profilename] [-B key=value] [-O builddir] [-D ret[=path]] [--no-strict]\n");
		return -1;
	}
	if(!getcwd(st.cwd, sizeof(st.cwd))) {
		perror("getcwd");
		return -1;
	}
	if(find_tup_dir() < 0 || open_tup_top() < 0) {
		fprintf(stderr, "tup error: 'tup gen' requires a tup hierarchy.\n");
		return -1;
	}
	if(!realpath(".", st.root)) {
		perror("realpath");
		return -1;
	}
	if(chdir(st.cwd) < 0) {
		perror("chdir");
		return -1;
	}
	st.root_meta_path = mt_join(st.root, "MetaTup.yaml");
	st.component_name = strdup(argv[0]);
	if(!st.root_meta_path || !st.component_name) {
		perror("strdup");
		goto out;
	}
	for(x=1; x<argc; x++) {
		if(strcmp(argv[x], "--no-strict") == 0) {
			st.strict = 0;
		} else if(strcmp(argv[x], "-P") == 0 && x + 1 < argc) {
			free(st.profile);
			st.profile = strdup(argv[++x]);
			if(!st.profile) {
				perror("strdup");
				goto out;
			}
		} else if(strcmp(argv[x], "-O") == 0 && x + 1 < argc) {
			free(st.override_builddir);
			st.override_builddir = strdup(argv[++x]);
		} else if(strcmp(argv[x], "-B") == 0 && x + 1 < argc) {
			char *eq;
			char *tmp = argv[++x];
			eq = strchr(tmp, '=');
			if(!eq) {
				fprintf(stderr, "tup error: -B expects key=value.\n");
				goto out;
			}
			{
				char *key = strndup(tmp, eq - tmp);
				if(!key) {
					perror("strndup");
					goto out;
				}
				if(mt_set_kv(&st.cli_args, &st.num_cli_args, key, eq + 1) < 0) {
					free(key);
					goto out;
				}
				free(key);
			}
		} else if(strcmp(argv[x], "-D") == 0 && x + 1 < argc) {
			if(mt_add_cli_dist(&st, argv[++x]) < 0)
				goto out;
		} else if(argv[x][0] == '-') {
			fprintf(stderr, "tup error: Unknown option '%s'.\n", argv[x]);
			goto out;
		} else if(!st.build_name_suffix) {
			st.build_name_suffix = strdup(argv[x]);
			if(!st.build_name_suffix) {
				perror("strdup");
				goto out;
			}
		} else {
			fprintf(stderr, "tup error: Unexpected argument '%s'.\n", argv[x]);
			goto out;
		}
	}
	if(mt_read_existing_tupbuild(&st) < 0)
		goto out;
	if(mt_load_file(st.root_meta_path, &root_file, &err) < 0) {
		fprintf(stderr, "%s\n", err ? err : "unable to load MetaTup.yaml");
		goto out;
	}
	root_component = mt_find_component(&root_file, st.component_name);
	if(!root_component) {
		fprintf(stderr, "tup error: Unable to find component '%s' in %s.\n", st.component_name, st.root_meta_path);
		goto out;
	}
	if(mt_clone_kvs(root_file.defaults, root_file.num_defaults, &root_args, &num_root_args) < 0)
		goto out;
	if(root_file.auto_compiledb_set)
		st.auto_compiledb = root_file.auto_compiledb;
	else if(st.have_existing_tb)
		st.auto_compiledb = st.existing_tb.auto_compiledb;
	if(st.profile) {
		struct mt_profile *profile = mt_find_profile(&root_file, st.profile);
		if(profile && mt_clone_kvs(profile->set, profile->num_set, &root_args, &num_root_args) < 0)
			goto out;
	}
	if(mt_clone_kvs(st.cli_args, st.num_cli_args, &root_args, &num_root_args) < 0)
		goto out;
	if(st.profile && mt_set_kv(&root_args, &num_root_args, st.profile, "true") < 0)
		goto out;
	if(st.build_name_suffix) {
		size_t len = strlen(st.component_name) + strlen(st.build_name_suffix) + 2;
		root_build_name = malloc(len);
		if(!root_build_name) {
			perror("malloc");
			goto out;
		}
		snprintf(root_build_name, len, "%s_%s", st.component_name, st.build_name_suffix);
	} else {
		root_build_name = strdup(st.component_name);
		if(!root_build_name) {
			perror("strdup");
			goto out;
		}
	}
	st.root_build_name = strdup(root_build_name);
	if(!st.root_build_name) {
		perror("strdup");
		goto out;
	}
	if(st.override_builddir) {
		root_builddir = strdup(st.override_builddir);
	} else {
		size_t len = strlen(root_build_name) + 9;
		root_builddir = malloc(len);
		if(root_builddir)
			snprintf(root_builddir, len, "./%s.build", root_build_name);
	}
	if(!root_builddir) {
		perror("malloc");
		goto out;
	}
	if(mt_build_component(&st, st.root, "", "", root_component,
			      root_build_name, root_builddir, root_build_name,
			      root_args, num_root_args, &root_build, &err) < 0) {
		fprintf(stderr, "%s\n", err ? err : "generation failed");
		goto out;
	}
	for(x=0; x<st.num_cli_dists; x++) {
		struct tupbuild_dist *tmp = realloc(root_build->dists, sizeof(*tmp) * (root_build->num_dists + 1));
		if(!tmp) {
			perror("realloc");
			goto out;
		}
		root_build->dists = tmp;
		root_build->dists[root_build->num_dists].from_return = strdup(st.cli_dists[x].from_return);
		if(st.cli_dists[x].path) {
			root_build->dists[root_build->num_dists].path = strdup(st.cli_dists[x].path);
		} else {
			size_t len = strlen(root_build->name) + strlen(st.cli_dists[x].from_return) + 9;
			root_build->dists[root_build->num_dists].path = malloc(len);
			if(root_build->dists[root_build->num_dists].path)
				snprintf(root_build->dists[root_build->num_dists].path, len, "./%s.%s.dist", root_build->name, st.cli_dists[x].from_return);
		}
		if(!root_build->dists[root_build->num_dists].from_return || !root_build->dists[root_build->num_dists].path) {
			perror("strdup");
			goto out;
		}
		root_build->num_dists++;
	}
	if(mt_write_tupbuild(&st) < 0)
		goto out;
	mt_free_kvs(root_args, num_root_args);
	mt_free_file(&root_file);
	mt_free_state(&st);
	free(root_build_name);
	free(root_builddir);
	free(err);
	return 0;
out:
	mt_free_kvs(root_args, num_root_args);
	mt_free_file(&root_file);
	mt_free_state(&st);
	free(root_build_name);
	free(root_builddir);
	free(err);
	return -1;
}
