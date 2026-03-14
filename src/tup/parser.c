/* vim: set ts=8 sw=8 sts=8 noet tw=78:
 *
 * tup - A file-based build system
 *
 * Copyright (C) 2008-2024  Mike Shal <marfey@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* _ATFILE_SOURCE needed at least on linux x86_64 */
#define _ATFILE_SOURCE
#include "parser.h"
#include "luaparser.h"
#include "progress.h"
#include "fileio.h"
#include "fslurp.h"
#include "db.h"
#include "environ.h"
#include "graph.h"
#include "config.h"
#include "bin.h"
#include "entry.h"
#include "container.h"
#include "if_stmt.h"
#include "server.h"
#include "variant.h"
#include "estring.h"
#include "tupbuild.h"
#include "flist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fnmatch.h>
#include <sys/stat.h>

#define SYNTAX_ERROR -2
#define CIRCULAR_DEPENDENCY_ERROR -3
#define ERROR_DIRECTIVE_ERROR -4
#define RETURN_DIRECTIVE -5

#define TUPFILE "Tupfile"
#define TUPDEFAULT "Tupdefault"
#define TUPFILE_LUA "Tupfile.lua"
#define TUPDEFAULT_LUA "Tupdefault.lua"
#define TUPBUILD "TupBuild.yaml"

struct bang_rule {
	struct string_tree st;
	int foreach;
	char *value;
	char *input;
	char *command;
	int command_len;
	struct path_list_head outputs;
	struct path_list_head extra_outputs;
};

struct tup_function {
	struct string_tree st;
	char *body;
	int line_number;
};

struct tup_function_registry {
	struct string_entries root;
};

struct tup_func_frame {
	struct vardb args;
	struct vardb vars;
	struct vardb returns;
	struct string_entries bang_root;
	struct bin_head bin_list;
	struct tup_func_frame *parent;
	struct tup_build_ctx *build_ctx;
	int has_return;
};

struct tup_build_ctx {
	int strict;
	struct tup_entry *caller_base_tent;
	struct string_entries caller_path_vars;
	struct path_list_head order_only_input_paths;
	struct name_list outputs;
};

struct dist_entry {
	char *src;
	char *dest;
};

struct dist_manifest {
	struct dist_entry *entries;
	int num_entries;
};

struct bang_list {
	TAILQ_ENTRY(bang_list) list;
	struct bang_rule *br;
};
TAILQ_HEAD(bang_list_head, bang_list);

struct build_name_list_args {
	struct name_list *nl;
	const char *globstr;  /* Pointer to the basename of the filename in the tupfile */
	int globstrlen;       /* Length of the basename */
	int wildcard;
	struct tupfile *tf;
	int orderid;
};

static int open_tupfile(struct tupfile *tf, struct tup_entry *tent,
			char *path, int *parser_type, int *fd);
static int parse_tupfile(struct tupfile *tf, struct buf *b, const char *filename);
static int scan_tupfile_functions(struct tupfile *tf, struct buf *b, const char *filename);
static int split_roots(struct tent_entries *root, struct graph *g);
static int parse_internal_definitions(struct tupfile *tf);
static int var_ifdef(struct tupfile *tf, const char *var);
static int eval_eq(struct tupfile *tf, char *expr, char *eol);
static int error_directive(struct tupfile *tf, char *cmdline);
static int inspect_directive(struct tupfile *tf, char *cmdline);
static int preload(struct tupfile *tf, char *cmdline);
static int run_script(struct tupfile *tf, char *cmdline, int lno);
static char *capture_run_script_output(struct tupfile *tf, const char *cmdline);
static char *eval_backticks(struct tupfile *tf, const char *string);
static int gitignore(struct tupfile *tf, struct tup_entry *dtent);
static int check_toplevel_gitignore(struct tupfile *tf);
static int parse_rule(struct tupfile *tf, char *p, int lno);
static int parse_bang_definition(struct tupfile *tf, char *p, int lno);
static int set_variable(struct tupfile *tf, char *line);
static int parse_bind(struct tupfile *tf, char *line);
static int parse_fbind(struct tupfile *tf, char *line, int lno);
static int parse_function_invoke(struct tupfile *tf, char *line, int lno, int inherit_reldir);
static int parse_return_stmt(struct tupfile *tf, char *line);
static int set_function_or_lua_var(struct tupfile *tf, const char *var, const char *value);
static int parse_rules_block(struct tupfile *tf, const char *body, int len, const char *filename);
static int parse_empty_bang_rule(struct tupfile *tf, struct rule *r);
static int parse_bang_rule(struct tupfile *tf, struct rule *r,
			   struct name_list *nl, const char *ext, int extlen);
static void free_bang_rule(struct string_entries *root, struct bang_rule *br);
static void free_bang_tree(struct string_entries *root);
static void free_dir_lists(struct string_entries *root);
static int split_input_pattern(struct tupfile *tf, char *p, char **o_input,
			       char **o_cmd, int *o_cmdlen, char **o_output,
			       char **o_bin);
static int parse_input_pattern(struct tupfile *tf, char *input_pattern,
			       struct name_list *inputs,
			       struct path_list_head *order_only_input_paths,
			       struct bin_head *bl,
			       int is_variant_copy);
static int parse_output_pattern(struct tupfile *tf, char *output_pattern,
				struct path_list_head *outputs,
				struct path_list_head *extra_outputs);
static int do_rule(struct tupfile *tf, struct rule *r, struct name_list *nl,
		   const char *ext, int extlen, struct name_list *output_nl);
static int path_list_to_nl(struct tupfile *tf, struct path_list_head *plist,
			   struct name_list *nl, struct name_list *input_nl,
			   int is_variant_copy);
static int get_path_list(struct tupfile *tf, const char *p, struct path_list_head *plist, struct bin_head *bl);
static int eval_path_list(struct tupfile *tf, struct path_list_head *plist, struct name_list *input_nl, int expand_nodes);
static int path_list_fill_dt_pel(struct tupfile *tf, struct path_list *pl, tupid_t dt, int create_output_dirs);
static int copy_path_list(struct tupfile *tf, struct path_list_head *dest, struct path_list_head *src);
static int nl_add_path(struct tupfile *tf, struct path_list *pl,
		       struct name_list *nl, int orderid,
		       int is_variant_copy);
static int nl_add_external_path(struct path_list *pl, struct name_list *nl, int orderid);
static int nl_add_bin(struct bin *b, struct name_list *nl, int orderid);
static int nl_rm_exclusion(struct tupfile *tf, struct path_list *pl, struct name_list *nl);
static int build_name_list_cb(void *arg, struct tup_entry *tent);
static void set_nle_base(struct name_list_entry *nle);
static void add_name_list_entry(struct name_list *nl,
				struct name_list_entry *nle);
static void delete_name_list_entry(struct name_list *nl,
				   struct name_list_entry *nle);
static char *tup_printf(struct tupfile *tf, const char *cmd, int cmd_len,
			struct name_list *nl, struct name_list *onl,
			struct name_list *ooinput_nl,
			const char *ext, int extlen,
			const char *extra_command,
			int percpercflag);

enum {
	NOEXPAND_PERCPERC,
	EXPAND_PERCPERC,
};

static int glob_parse(const char *base, int baselen, char *expanded, int *globidx);
static int next_path(struct tupfile *tf, const char *p, char *dest);
static int char_find(const char *s, int len, const char *list);
static int parse_function_definition(struct tupfile *tf, char *line, const char *body, int body_len, int lno);
static int skip_curly_block(char **pp, char *e, int *lno, char **body_start, int *body_len);
static int line_starts_with_trimmed(const char *line, int len, const char *prefix);
static int line_is_trimmed_close_brace(const char *line, int len);
static int parse_quoted_string(const char **sp, char **out);
static int parse_simple_fbind_var(const char *text, char **out);
static int parse_materialize_args(struct tupfile *tf, const char *text, char **spec, char **root,
				  int *root_from_caller_root);
static struct string_tree *find_bang_rule(struct tupfile *tf, const char *name, int len);
static void free_function_registry(struct tup_function_registry *reg);
static int function_registry_init(struct tup_function_registry *reg);
static const char *find_matching_paren(const char *s);
static int parse_arg_map(struct tupfile *tf, const char *text, struct vardb *args, int allow_spread);
static int execute_function(struct tupfile *tf, struct tup_function *fn, struct vardb *args,
			    struct tup_build_ctx *ctx, const char *filename, int inherit_reldir,
			    struct vardb *returns);
static int load_function_file(struct tupfile *tf, const char *file, struct tup_function_registry *reg,
			      struct tup_entry **oldtent, int *old_dfd, struct tup_entry **loadedtent);
static void restore_loaded_function_file(struct tupfile *tf, struct tup_entry *oldtent, int old_dfd);
static int vardb_clone(struct vardb *dst, struct vardb *src);
static int function_arg_set_reldir(struct tupfile *tf, struct vardb *args);
static int function_arg_set_brdir(struct vardb *args);
static int parse_tupbuild(struct tupfile *tf, struct buf *b, const char *filename);
static int tupbuild_resolve_builddir(struct tupfile *tf, const char *builddir, struct tup_entry **tent);
static int tupbuild_combine_builddir_reldir(const char *builddir, const char *reldir, char **out);
static int canonicalize_path_simple(const char *path, char **out);
static int eval_abs_function(struct tupfile *tf, const char *args, int argslen, struct estring *e);
static int eval_globs_function(struct tupfile *tf, const char *args, int argslen, struct estring *e);
static int eval_groups_function(struct tupfile *tf, const char *args, int argslen, struct estring *e);
static int eval_realname_function(struct tupfile *tf, const char *args, int argslen, struct estring *e);
static int move_name_list(struct name_list *dst, struct name_list *src);
static int build_ctx_strict_check(struct tupfile *tf, struct path_list *pl);
static int build_ctx_copy_order_only_inputs(struct tupfile *tf, struct rule *r);
static int build_ctx_add_caller_path_var(struct tup_build_ctx *ctx, const char *name);
static char *dup_trimmed_range(const char *start, const char *end);
static void dist_manifest_free(struct dist_manifest *dm);
static int dist_parse_manifest(struct tupfile *tf, const char *spec, struct dist_manifest *dm);
static int tupbuild_materialize_dist(struct tupfile *tf, struct tup_build_ctx *ctx,
				     const char *build_name, const char *root,
				     const char *spec, int root_from_caller_root);
static int path_uses_caller_path_var(struct tupfile *tf, const char *path);
static int statement_needs_more_lines(const char *line);
static struct bin_head *current_bin_head(struct tupfile *tf);
static int tent_is_tup_internal(struct tup_entry *tent);
static int resolve_materialized_dir(struct tupfile *tf, const char *path,
				    int root_from_caller_root, struct tup_entry **tent);

enum parser_type {
	PARSER_TYPE_TUP = 0,
	PARSER_TYPE_LUA = 1,
	PARSER_TYPE_YAML = 2,
};

static int debug_run = 0;
static int parser_auto_compiledb = 0;

void parser_debug_run(void)
{
	debug_run = 1;
	lua_parser_debug_run();
}

void parser_reset_tupbuild_flags(void)
{
	parser_auto_compiledb = 0;
}

int parser_get_auto_compiledb(void)
{
	return parser_auto_compiledb;
}

int parse(struct node *n, struct graph *g, struct timespan *retts, int refactoring, int use_server, int full_deps)
{
	struct tupfile tf;
	int fd = -1;
	int rc = -1;
	int parser_type = PARSER_TYPE_TUP;
	struct buf b = {NULL, 0};
	struct parser_server ps;
	struct timeval orig_start;
	char path[PATH_MAX];

	if(is_virtual_tent(n->tent))
		return 0;

	timespan_start(&tf.ts);
	memcpy(&orig_start, &tf.ts.start, sizeof(orig_start));
	if(snprint_tup_entry(path, sizeof(path), n->tent) < (signed)sizeof(path) &&
	   (strcmp(path, ".metatup") == 0 || strncmp(path, ".metatup/", 9) == 0)) {
		timespan_end(&tf.ts);
		*retts = tf.ts;
		return 0;
	}
	if(n->parsing) {
		fprintf(stderr, "tup error: Circular dependency found among Tupfiles. Last directory: ");
		print_tup_entry(stderr, n->tent);
		fprintf(stderr, "\n");
		return CIRCULAR_DEPENDENCY_ERROR;
	}
	n->parsing = 1;
	if(tent_is_tup_internal(n->tent)) {
		timespan_end(&tf.ts);
		*retts = tf.ts;
		n->parsing = 0;
		return 0;
	}

	tf.variant = tup_entry_variant(n->tent);
	tf.ps = &ps;
	tf.f = tmpfile();
	if(!tf.f) {
		perror("tmpfile");
		return -1;
	}
	tf.luaerror = TUPLUA_NOERROR;
	tf.use_server = use_server;

	/* We may need to convert normal dirs back to generated dirs,
	 * so add this one to check.
	 */
	if(tf.variant->root_variant) {
		if(tent_tree_add_dup(&g->normal_dir_root, n->tent) < 0)
			return -1;
	}

	init_file_info(&ps.s.finfo, 0);
	ps.s.id = n->tnode.tupid;
	pthread_mutex_init(&ps.lock, NULL);

	RB_INIT(&tf.cmd_root);
	tent_tree_init(&tf.env_root);
	RB_INIT(&tf.bang_root);
	tent_tree_init(&tf.input_root);
	RB_INIT(&tf.directory_root);
	RB_INIT(&ps.directories);
	tent_tree_init(&tf.refactoring_cmd_delete_root);

	if(tf.use_server) {
		if(server_parser_start(&ps) < 0)
			return -1;
	} else {
		ps.root_fd = tup_top_fd();
	}

	tf.tent = n->tent;
	tf.curtent = tf.tent;
	tf.srctent = variant_tent_to_srctent(n->tent);
	tf.root_fd = ps.root_fd;
	tf.g = g;
	tf.refactoring = refactoring;
	tf.full_deps = full_deps;
	tf.including_rules = 0;
	tf.in_function_body = 0;
	tf.in_rules_block = 0;
	tf.function_registry = NULL;
	tf.func_frame = NULL;
	tf.ign = 0;
	tf.circular_dep_error = 0;
	LIST_INIT(&tf.bin_list);
	if(vardb_init(&tf.node_db) < 0)
		goto out_server_stop;
	if(environ_add_defaults(&tf.env_root) < 0)
		goto out_close_vdb;

	/* Keep track of the commands and generated files that we had created
	 * previously. We'll check these against the new ones in order to see
	 * if any should be removed.
	 */
	if(tup_db_dirtype(tf.tent->tnode.tupid, NULL, &g->cmd_delete_root, TUP_NODE_CMD) < 0)
		goto out_close_vdb;

	struct tent_entries gen_delete_root = TENT_ENTRIES_INITIALIZER;
	if(tup_db_srcid_to_tree(tf.tent->tnode.tupid, &gen_delete_root, TUP_NODE_GENERATED) < 0)
		goto out_close_vdb;
	if(split_roots(&gen_delete_root, g) < 0)
		goto out_close_vdb;

	if(refactoring) {
		if(tup_db_dirtype(tf.tent->tnode.tupid, NULL, &tf.refactoring_cmd_delete_root, TUP_NODE_CMD) < 0)
			goto out_close_vdb;
	}

	if(parse_internal_definitions(&tf) < 0) {
		goto out_close_vdb;
	}

	tf.cur_dfd = tup_entry_openat(ps.root_fd, tf.srctent);
	if(tf.cur_dfd < 0) {
		if(tent_is_tup_internal(n->tent) &&
		   (errno == EACCES || errno == EPERM || errno == ENOTCONN)) {
			rc = 0;
			goto out_close_vdb;
		}
		fprintf(tf.f, "tup error: Unable to open directory: ");
		print_tup_entry(tf.f, tf.srctent);
		fprintf(tf.f, "\n");
		goto out_close_vdb;
	}

	if(open_tupfile(&tf, n->tent, path, &parser_type, &fd) < 0)
		goto out_close_dfd;
	if(fd < 0) {
		/* No Tupfile means we have nothing to do */
		if(n->tent->tnode.tupid == DOT_DT) {
			/* Check to see if the top-level rules file would .gitignore. We disable
			 * tf.tent->tnode.tupid so no rules get created.
			 */
			if(check_toplevel_gitignore(&tf) < 0)
				goto out_close_dfd;
		}
	}
	push_tupfile(&tf);
	if(fd >= 0) {
		int tmprc;
		tmprc = fslurp_null(fd, &b);
		if(close(fd) < 0) {
			parser_error(&tf, "close(fd)");
			tmprc = -1;
		}
		if(tmprc < 0)
			goto out_free_bs;
		if(parser_type == PARSER_TYPE_TUP) {
			tf.function_registry = malloc(sizeof *tf.function_registry);
			if(!tf.function_registry) {
				perror("malloc");
				goto out_free_bs;
			}
			function_registry_init(tf.function_registry);
			if(parse_tupfile(&tf, &b, "Tupfile") < 0)
				goto out_free_bs;
		} else if(parser_type == PARSER_TYPE_LUA) {
			if(parse_lua_include_rules(&tf) < 0)
				goto out_free_bs;
			if(parse_lua_tupfile(&tf, &b, path) < 0)
				goto out_free_bs;
		} else {
			if(parse_tupbuild(&tf, &b, path) < 0)
				goto out_free_bs;
		}
	}
	pop_tupfile();
	if(tf.ign) {
		if(!tf.variant->root_variant) {
			if(n->tent->srcid == DOT_DT) {
				struct tup_entry *srctent;
				if(tup_entry_add(n->tent->srcid, &srctent) < 0)
					return -1;
				if(gitignore(&tf, srctent) < 0) {
					goto out_free_bs;
				}
			}
		}
		if(gitignore(&tf, tf.tent) < 0) {
			goto out_free_bs;
		}
	} else {
		struct tup_entry *tent;
		if(tup_db_select_tent(tf.tent, ".gitignore", &tent) < 0)
			goto out_free_bs;
		if(tent && tent->type == TUP_NODE_GENERATED) {
			if(refactoring) {
				fprintf(tf.f, "tup refactoring error: Attempting to remove the .gitignore file.\n");
				goto out_free_bs;
			}
			if(remove_tup_gitignore(tf.f, tf.g, tent) < 0)
				goto out_free_bs;
		}
	}
	rc = 0;
out_free_bs:
	free(b.s);
out_close_dfd:
	if(tf.cur_dfd >= 0 && close(tf.cur_dfd) < 0) {
		parser_error(&tf, "close(tf.cur_dfd)");
		rc = -1;
	}
out_close_vdb:
	if(vardb_close(&tf.node_db) < 0)
		rc = -1;
out_server_stop:
	if(tf.function_registry)
		free_function_registry(tf.function_registry);
	bin_list_del(&tf.bin_list);
	if(tf.use_server)
		if(server_parser_stop(&ps) < 0)
			rc = -1;

	if(rc == 0) {
		if(refactoring) {
			struct tent_tree *tt;

			RB_FOREACH(tt, tent_entries, &tf.refactoring_cmd_delete_root) {
				rc = -1;
				fprintf(tf.f, "tup refactoring error: Attempting to delete a command: ");
				print_tup_entry(tf.f, tt->tent);
				fprintf(tf.f, "\n");
			}
		}
		if(add_parser_files(&ps.s.finfo, &tf.input_root, tf.variant->tent->tnode.tupid, full_deps) < 0)
			rc = -1;
		if(tup_db_write_dir_inputs(tf.f, tf.tent->tnode.tupid, &tf.input_root) < 0)
			rc = -1;
	}

	pthread_mutex_lock(&ps.lock);
	free_dir_lists(&ps.directories);
	pthread_mutex_unlock(&ps.lock);

	free_tent_tree(&tf.env_root);
	free_tupid_tree(&tf.cmd_root);
	free_tupid_tree(&tf.directory_root);
	free_bang_tree(&tf.bang_root);
	free_tent_tree(&tf.input_root);

	timespan_end(&tf.ts);
	if(retts) {
		/* Report back the original start time, and our real end time.
		 * This accounts for the total time to parse us, plus any
		 * dependent Tupfiles. If another Tupfile depends on us, they
		 * can subtract this full time out from their reported time.
		 */
		memcpy(&retts->start, &orig_start, sizeof(retts->start));
		memcpy(&retts->end, &tf.ts.end, sizeof(retts->end));
	}
	show_result(n->tent, rc != 0, &tf.ts, NULL, 0);
	if(fflush(tf.f) != 0) {
		/* Use perror, since we're trying to flush the tf.f output */
		perror("fflush");
		rc = -1;
	}
	rewind(tf.f);
	display_output(fileno(tf.f), rc == 0 ? 0 : 3, NULL, 0, NULL);
	if(fclose(tf.f) != 0) {
		/* Use perror, since we're trying to close the tf.f output */
		perror("fclose");
		rc = -1;
	}
	if(tf.circular_dep_error)
		rc = CIRCULAR_DEPENDENCY_ERROR;

	return rc;
}

static int open_if_entry(struct tupfile *tf, struct tup_entry *dtent, const char *fullpath, const char *path, int *fd)
{
	struct tup_entry *tupfile_tent;
	if(handle_file_dtent(ACCESS_READ, dtent, path, &tf->ps->s.finfo) < 0)
		return -1;
	if(tup_db_select_tent(dtent, path, &tupfile_tent) < 0)
		return -1;
	if(!tupfile_tent) {
		if(tup_db_node_insert_tent(dtent, path, strlen(path), TUP_NODE_GHOST, INVALID_MTIME, -1, &tupfile_tent) < 0) {
			fprintf(tf->f, "tup error: Node '%s' doesn't exist and we couldn't create a ghost in directory: ", path);
			print_tup_entry(tf->f, dtent);
			fprintf(tf->f, "\n");
			return -1;
		}
	}
	if(tupfile_tent->type == TUP_NODE_GHOST) {
		if(tent_tree_add_dup(&tf->input_root, tupfile_tent) < 0)
			return -1;
		return 0;
	}
	*fd = openat(tf->cur_dfd, fullpath, O_RDONLY);
	if(*fd < 0 && errno != ENOENT) {
		parser_error(tf, fullpath);
		return -1;
	}
	return 0;
}

static int parser_entry_open(struct tupfile *tf, struct tup_entry *tent)
{
	int fd;
	if(handle_file_dtent(ACCESS_READ, tent->parent, tent->name.s, &tf->ps->s.finfo) < 0) {
		parser_error(tf, tent->name.s);
		return -1;
	}
	fd = tup_entry_openat(tf->root_fd, tent);
	if(fd < 0) {
		parser_error(tf, tent->name.s);
		return -1;
	}
	return fd;
}

static int open_tupfile(struct tupfile *tf, struct tup_entry *tent,
			char *path, int *parser_type, int *fd)
{
	struct tup_entry *dtent;
	int n = 0;

	if(tf->variant->root_variant) {
		dtent = tent;
	} else {
		if(tent->srcid < 0) {
			/* This happens if there is a manually created
			 * directory inside a variant (t8075).
			 */
			return 0;
		}
		if(tup_entry_add(tent->srcid, &dtent) < 0)
			return -1;
	}

	strcpy(path, TUPFILE);
	if(open_if_entry(tf, dtent, path, TUPFILE, fd) < 0)
		return -1;
	if(*fd >= 0) {
		return 0;
	}

	strcpy(path, TUPFILE_LUA);
	if(open_if_entry(tf, dtent, path, TUPFILE_LUA, fd) < 0)
		return -1;
	if(*fd >= 0) {
		*parser_type = PARSER_TYPE_LUA;
		return 0;
	}

	strcpy(path, TUPBUILD);
	if(open_if_entry(tf, dtent, path, TUPBUILD, fd) < 0)
		return -1;
	if(*fd >= 0) {
		*parser_type = PARSER_TYPE_YAML;
		return 0;
	}
	do {
		int x;
		strcpy(path + n*3, TUPDEFAULT);
		if(open_if_entry(tf, dtent, path, TUPDEFAULT, fd) < 0)
			return -1;
		if(*fd >= 0) {
			return 0;
		}

		strcpy(path + n*3, TUPDEFAULT_LUA);
		if(open_if_entry(tf, dtent, path, TUPDEFAULT_LUA, fd) < 0)
			return -1;
		if(*fd >= 0) {
			*parser_type = PARSER_TYPE_LUA;
			return 0;
		}

		n++;
		for(x=0; x<n; x++) {
			strcpy(path + x*3, "../");
		}
		dtent = dtent->parent;
	} while(dtent);

	return 0;
}

static int split_roots(struct tent_entries *root, struct graph *g)
{
	while(!RB_EMPTY(root)) {
		struct tent_tree *tt = RB_MIN(tent_entries, root);
		int mod;
		tent_tree_remove(root, tt->tent);
		mod = tup_db_in_modify_list(tt->tent->tnode.tupid);
		if(mod < 0)
			return -1;
		if(mod) {
			if(tent_tree_add(&g->save_root, tt->tent) < 0)
				return -1;
		} else {
			if(tent_tree_add(&g->gen_delete_root, tt->tent) < 0)
				return -1;
		}
	}
	return 0;
}

#define TUP_PRESERVE_CMD "!tup_preserve %f %o"
#define TUP_LN_CMD       "!tup_ln %f %o"

static int parse_internal_definitions(struct tupfile *tf)
{
	char tup_ln[] = "!tup_ln = |> ^ symlink %o -> %f^ " TUP_LN_CMD " |>";
	if(parse_bang_definition(tf, tup_ln, 0) < 0) {
		fprintf(tf->f, "tup error: Unable to parse built-in !tup_ln rule.\n");
		return -1;
	}
	if(tf->variant->root_variant) {
		char tup_preserve[] = "!tup_preserve = |> |>";
		if(parse_bang_definition(tf, tup_preserve, 0) < 0)
			return -1;
	} else {
		char tup_preserve[] = "!tup_preserve = |> ^ preserve %o^ " TUP_PRESERVE_CMD " |> %b";
		if(parse_bang_definition(tf, tup_preserve, 0) < 0)
			return -1;
	}
	return 0;
}

static char *get_newline(char *p)
{
	char *newline;

	newline = strchr(p, '\n');
	if(!newline) {
		newline = strchr(p, '\0');
	}
	return newline;
}

static int statement_needs_more_lines(const char *line)
{
	int paren = 0;
	int brace = 0;
	int bracket = 0;
	int in_quote = 0;
	int escaped = 0;

	while(*line) {
		if(in_quote) {
			if(escaped) {
				escaped = 0;
			} else if(*line == '\\') {
				escaped = 1;
			} else if(*line == '"') {
				in_quote = 0;
			}
		} else {
			if(*line == '"') {
				in_quote = 1;
			} else if(*line == '(') {
				paren++;
			} else if(*line == ')') {
				paren--;
			} else if(*line == '{') {
				brace++;
			} else if(*line == '}') {
				brace--;
			} else if(*line == '[') {
				bracket++;
			} else if(*line == ']') {
				bracket--;
			}
		}
		line++;
	}

	return in_quote || paren > 0 || brace > 0 || bracket > 0;
}

static struct bin_head *current_bin_head(struct tupfile *tf)
{
	if(tf->func_frame)
		return &tf->func_frame->bin_list;
	return &tf->bin_list;
}

static int tent_is_tup_internal(struct tup_entry *tent)
{
	char path[PATH_MAX];
	int rc;

	rc = snprint_tup_entry(path, sizeof(path), tent);
	if(rc < 0 || rc >= (int)sizeof(path))
		return 0;
	return strcmp(path, ".metatup") == 0 || strncmp(path, ".metatup/", 9) == 0;
}

static int line_starts_with_trimmed(const char *line, int len, const char *prefix)
{
	int plen = strlen(prefix);

	while(len > 0 && isspace(*line)) {
		line++;
		len--;
	}
	if(len < plen)
		return 0;
	return strncmp(line, prefix, plen) == 0;
}

static int line_is_trimmed_close_brace(const char *line, int len)
{
	while(len > 0 && isspace(*line)) {
		line++;
		len--;
	}
	while(len > 0 && isspace(line[len-1]))
		len--;
	return len == 1 && line[0] == '}';
}

static int skip_curly_block(char **pp, char *e, int *lno, char **body_start, int *body_len)
{
	char *p = *pp;
	char *start = p;
	int depth = 1;

	if(body_start)
		*body_start = p;
	while(p < e) {
		char *newline;
		char *line;
		int len;

		line = p;
		newline = get_newline(p);
		if(!newline)
			return -1;
		if(*newline == '\n')
			(*lno)++;
		len = newline - line;

		if(line_starts_with_trimmed(line, len, "call ") ||
		   line_starts_with_trimmed(line, len, "spawn ") ||
		   line_starts_with_trimmed(line, len, "bind ") ||
		   line_starts_with_trimmed(line, len, "fbind ") ||
		   line_starts_with_trimmed(line, len, "return ") ||
		   line_starts_with_trimmed(line, len, "inspect ")) {
			char saved = *newline;

			*newline = 0;
			while(statement_needs_more_lines(line)) {
				if(saved == 0 || p >= e)
					return -1;
				*newline = ' ';
				newline = get_newline(newline + 1);
				if(!newline)
					return -1;
				saved = *newline;
				*newline = 0;
				if(saved == '\n')
					(*lno)++;
			}
			*newline = saved;
			p = newline + 1;
			continue;
		}

		if(line_starts_with_trimmed(line, len, "function ") ||
		   line_starts_with_trimmed(line, len, "rules {")) {
			depth++;
		} else if(line_is_trimmed_close_brace(line, len)) {
			depth--;
			if(depth == 0) {
				if(body_len)
					*body_len = line - start;
				*pp = newline + 1;
				return 0;
			}
		}
		p = newline + 1;
	}
	return -1;
}

static int function_registry_init(struct tup_function_registry *reg)
{
	RB_INIT(&reg->root);
	return 0;
}

static void free_function_registry(struct tup_function_registry *reg)
{
	struct string_tree *st;

	if(!reg)
		return;
	while((st = RB_ROOT(&reg->root)) != NULL) {
		struct tup_function *fn = container_of(st, struct tup_function, st);
		string_tree_remove(&reg->root, &fn->st);
		free(fn->body);
		free(fn);
	}
	free(reg);
}

static int parse_function_definition(struct tupfile *tf, char *line, const char *body, int body_len, int lno)
{
	char *name;
	char *brace;
	struct string_tree *st;
	struct tup_function *fn;

	if(!tf->function_registry) {
		fprintf(tf->f, "tup internal error: no function registry available.\n");
		return -1;
	}
	name = line + 8;
	while(isspace(*name))
		name++;
	brace = strrchr(name, '{');
	if(!brace)
		return SYNTAX_ERROR;
	brace--;
	while(brace >= name && isspace(*brace)) {
		*brace = 0;
		brace--;
	}
	if(name[0] == 0)
		return SYNTAX_ERROR;

	st = string_tree_search(&tf->function_registry->root, name, strlen(name));
	if(st) {
		fprintf(tf->f, "tup error: Duplicate function '%s'.\n", name);
		return -1;
	}

	fn = malloc(sizeof *fn);
	if(!fn) {
		perror("malloc");
		return -1;
	}
	fn->body = malloc(body_len + 1);
	if(!fn->body) {
		perror("malloc");
		free(fn);
		return -1;
	}
	memcpy(fn->body, body, body_len);
	fn->body[body_len] = 0;
	fn->line_number = lno;
	if(string_tree_add(&tf->function_registry->root, &fn->st, name) < 0) {
		fprintf(tf->f, "tup internal error: unable to add function '%s'.\n", name);
		free(fn->body);
		free(fn);
		return -1;
	}
	return 0;
}

static int scan_tupfile_functions(struct tupfile *tf, struct buf *b, const char *filename)
{
	char *scan;
	char *p;
	char *e;
	int lno = 0;

	if(!tf->function_registry)
		return 0;
	scan = malloc(b->len + 1);
	if(!scan) {
		perror("malloc");
		return -1;
	}
	memcpy(scan, b->s, b->len + 1);
	p = scan;
	e = scan + b->len;
	while(p < e) {
		char *newline;
		char *line;
		char *trim;
		char *body;
		int body_len;

		while(p < e && isspace(*p)) {
			if(*p == '\n')
				lno++;
			p++;
		}
		if(p == e)
			break;
		line = p;
		newline = get_newline(p);
		if(!newline) {
			free(scan);
			fprintf(tf->f, "tup error: Unable to find trailing nul-byte in %s.\n", filename);
			return -1;
		}
		lno++;
		p = newline + 1;
		while(newline > line && isspace(newline[-1]))
			newline--;
		*newline = 0;
		if(line[0] == '#')
			continue;
		trim = line;
		while(isspace(*trim))
			trim++;
		if(strncmp(trim, "function ", 9) == 0) {
			if(skip_curly_block(&p, e, &lno, &body, &body_len) < 0) {
				free(scan);
				fprintf(tf->f, "tup error: Function block missing closing brace in %s.\n", filename);
				return -1;
			}
			if(parse_function_definition(tf, trim, body, body_len, lno) < 0) {
				free(scan);
				return -1;
			}
		}
	}
	free(scan);
	return 0;
}

static int parse_tupfile(struct tupfile *tf, struct buf *b, const char *filename)
{
	char *p, *e;
	char *line;
	int lno = 0;
	struct if_stmt ifs;
	int rc = 0;
	char line_debug[128];

	if_init(&ifs);

	if(tf->function_registry && !tf->in_function_body && !tf->in_rules_block)
		if(scan_tupfile_functions(tf, b, filename) < 0)
			return -1;

	p = b->s;
	e = b->s + b->len;

	while(p < e) {
		char *newline;

		/* Skip leading whitespace and empty lines */
		while(p < e && isspace(*p)) {
			if(*p == '\n')
				lno++;
			p++;
		}
		/* If we just had empty lines at the end, we're done */
		if(p == e)
			break;

		line = p;
		newline = get_newline(p);
		if(!newline) {
			fprintf(tf->f, "tup error: Unable to find trailing nul-byte.\n");
			fprintf(tf->f, "  Line was: '%s'\n", line);
			return -1;
		}
		lno++;
		while(newline[-1] == '\\' || (newline[-2] == '\\' && newline[-1] == '\r')) {
			if (newline[-1] == '\r') {
				newline[-2] = ' ';
			}
			newline[-1] = ' ';
			newline[0] = ' ';
			newline = get_newline(p);
			if(!newline) {
				fprintf(tf->f, "tup error: Unable to find trailing nul-byte.\n");
				fprintf(tf->f, "  Line was: '%s'\n", line);
				return -1;
			}
			lno++;
		}
		if(strncmp(line, "call ", 5) == 0 || strncmp(line, "spawn ", 6) == 0 ||
		   strncmp(line, "bind ", 5) == 0 || strncmp(line, "fbind ", 6) == 0 ||
		   strncmp(line, "return ", 7) == 0 ||
		   strncmp(line, "inspect ", 8) == 0) {
			char saved = *newline;

			*newline = 0;
			while(statement_needs_more_lines(line)) {
				if(saved == 0 || p >= e) {
					fprintf(tf->f, "tup error: Unterminated multi-line statement.\n");
					fprintf(tf->f, "  Line was: '%s'\n", line);
					return -1;
				}
				*newline = ' ';
				newline = get_newline(newline + 1);
				if(!newline) {
					fprintf(tf->f, "tup error: Unable to find trailing nul-byte.\n");
					fprintf(tf->f, "  Line was: '%s'\n", line);
					return -1;
				}
				saved = *newline;
				*newline = 0;
				if(saved == '\n')
					lno++;
			}
			*newline = 0;
		}

		p = newline + 1;

		/* Remove trailing whitespace */
		while(newline > line && isspace(newline[-1])) {
			newline--;
		}
		*newline = 0;

		if(line[0] == '#') {
			/* Skip comments */
			continue;
		}

		strncpy(line_debug, line, sizeof(line_debug) - 1);
		memcpy(line_debug + sizeof(line_debug) - 4, "...", 4);

		rc = 0;
		if(strcmp(line, "else") == 0) {
			rc = if_else(&ifs);
		} else if(strcmp(line, "endif") == 0) {
			rc = if_endif(&ifs);
		} else if(strncmp(line, "ifeq ", 5) == 0) {
			if(if_true(&ifs)) {
				rc = eval_eq(tf, line+5, newline);
			}
			if(rc >= 0)
				rc = if_add(&ifs, rc);
		} else if(strncmp(line, "ifneq ", 6) == 0) {
			if(if_true(&ifs)) {
				rc = eval_eq(tf, line+6, newline);
			}
			if(rc >= 0)
				rc = if_add(&ifs, !rc);
		} else if(strncmp(line, "ifdef ", 6) == 0) {
			if(if_true(&ifs)) {
				rc = var_ifdef(tf, line+6);
			}
			if(rc >= 0)
				rc = if_add(&ifs, rc);
		} else if(strncmp(line, "ifndef ", 7) == 0) {
			if(if_true(&ifs)) {
				rc = var_ifdef(tf, line+7);
			}
			if(rc >= 0)
				rc = if_add(&ifs, !rc);
		} else if(!if_true(&ifs)) {
			/* Skip the false part of an if block */
		} else if(strncmp(line, "function ", 9) == 0) {
			if(tf->in_function_body || tf->in_rules_block) {
				rc = SYNTAX_ERROR;
			} else {
				char *body;
				int body_len;
				if(skip_curly_block(&p, e, &lno, &body, &body_len) < 0) {
					fprintf(tf->f, "tup error: Function block missing closing brace.\n");
					return -1;
				}
				rc = 0;
			}
		} else if(strncmp(line, "rules {", 7) == 0) {
			char *body;
			int body_len;

			if(!tf->in_function_body || !tf->func_frame) {
				rc = SYNTAX_ERROR;
			} else {
				if(skip_curly_block(&p, e, &lno, &body, &body_len) < 0) {
					fprintf(tf->f, "tup error: rules block missing closing brace.\n");
					return -1;
				}
				rc = parse_rules_block(tf, body, body_len, filename);
			}
		} else if(strncmp(line, "error ", 6) == 0) {
			rc = error_directive(tf, line+6);
		} else if(strncmp(line, "inspect ", 8) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = inspect_directive(tf, line+8);
		} else if(strncmp(line, "call ", 5) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = parse_function_invoke(tf, line+5, lno, 1);
		} else if(strncmp(line, "spawn ", 6) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = parse_function_invoke(tf, line+6, lno, 0);
		} else if(strncmp(line, "bind ", 5) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = parse_bind(tf, line+5);
		} else if(strncmp(line, "fbind ", 6) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = parse_fbind(tf, line+6, lno);
		} else if(strncmp(line, "return ", 7) == 0) {
			if(tf->in_rules_block || !tf->in_function_body || !tf->func_frame)
				rc = SYNTAX_ERROR;
			else
				rc = parse_return_stmt(tf, line+7);
		} else if(strncmp(line, "include ", 8) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else {
			char *file;

			file = line + 8;
			file = eval(tf, file, EXPAND_NODES);
			if(!file) {
				rc = -1;
			} else {
				rc = parser_include_file(tf, file);
				free(file);
			}
			}
		} else if(strcmp(line, "include_rules") == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = parser_include_rules(tf, "Tuprules.tup");
		} else if(strncmp(line, "preload ", 8) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = preload(tf, line+8);
		} else if(strncmp(line, "run ", 4) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = run_script(tf, line+4, lno);
		} else if(strncmp(line, "export ", 7) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = export(tf, line+7);
		} else if(strncmp(line, "import ", 7) == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = import(tf, line+7, NULL, NULL);
		} else if(strcmp(line, ".gitignore") == 0) {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				tf->ign = 1;
		} else if(line[0] == ':') {
			if(tf->in_rules_block)
				rc = SYNTAX_ERROR;
			else
				rc = parse_rule(tf, line+1, lno);
		} else if(line[0] == '!') {
			rc = parse_bang_definition(tf, line, lno);
		} else {
			rc = set_variable(tf, line);
		}

		if(rc == ERROR_DIRECTIVE_ERROR) {
			fprintf(tf->f, "tup error: Found 'error' command parsing %s line %i. Quitting.\n", filename, lno);
			return -1;
		}
		if(rc == RETURN_DIRECTIVE)
			return 0;
		if(rc == SYNTAX_ERROR) {
			fprintf(tf->f, "tup error: Syntax error parsing %s line %i\n  Line was: '%s'\n", filename, lno, line_debug);
			return -1;
		}
		if(rc < 0) {
			fprintf(tf->f, "tup error: Error parsing %s line %i\n  Line was: '%s'\n", filename, lno, line_debug);
			return -1;
		}
	}
	if(if_check(&ifs) < 0) {
		fprintf(tf->f, "tup error: Error parsing %s: missing endif before EOF.\n", filename);
		return -1;
	}

	return 0;
}

static int eval_eq(struct tupfile *tf, char *expr, char *eol)
{
	char *paren;
	char *comma;
	char *lval;
	char *rval;
	int rc;

	paren = strchr(expr, '(');
	if(!paren)
		return SYNTAX_ERROR;
	lval = paren + 1;
	comma = strchr(lval, ',');
	if(!comma)
		return SYNTAX_ERROR;
	rval = comma + 1;

	paren = eol;
	while(paren > expr) {
		if(*paren == ')')
            goto found_paren;
		paren--;
	}
	return SYNTAX_ERROR;
found_paren:
	*comma = 0;
	*paren = 0;

	lval = eval(tf, lval, KEEP_NODES);
	if(!lval)
		return -1;
	rval = eval(tf, rval, KEEP_NODES);
	if(!rval) {
		free(lval);
		return -1;
	}

	if(strcmp(lval, rval) == 0)
		rc = 1;
	else
		rc = 0;
	free(lval);
	free(rval);

	return rc;
}

static int var_ifdef(struct tupfile *tf, const char *var)
{
	struct tup_entry *tent;
	int rc;

	if(strncmp(var, "CONFIG_", 7) == 0)
		var += 7;
	tent = tup_db_get_var(tf->variant, var, strlen(var), NULL);
	if(!tent)
		return -1;
	if(tent->type == TUP_NODE_VAR) {
		rc = 1;
	} else {
		rc = 0;
	}
	if(tent_tree_add_dup(&tf->input_root, tent) < 0)
		return -1;
	return rc;
}

static int error_directive(struct tupfile *tf, char *cmdline)
{
	char *eval_cmdline;
	eval_cmdline = eval(tf, cmdline, KEEP_NODES);
	if(eval_cmdline) {
		fprintf(tf->f, "Error:\n  ");
		if(strlen(cmdline)==0) {
			fprintf(tf->f, "Empty error directive\n");
		} else {
			fprintf(tf->f, "%s\n", eval_cmdline);

		}
		free(eval_cmdline);
	} else {
		fprintf(tf->f, "Unable to expand 'error' message. Raw error string:\n  %s\n", cmdline);
	}
	return ERROR_DIRECTIVE_ERROR;
}

static int inspect_directive(struct tupfile *tf, char *cmdline)
{
	char *eval_cmdline;
	char *msg;
	int len;

	eval_cmdline = eval(tf, cmdline, KEEP_NODES);
	if(!eval_cmdline) {
		fprintf(stderr, "Unable to expand 'inspect' message. Raw inspect string:\n  %s\n", cmdline);
		return -1;
	}
	msg = eval_cmdline;
	len = strlen(msg);
	if(len >= 2 && ((msg[0] == '"' && msg[len-1] == '"') ||
		        (msg[0] == '\'' && msg[len-1] == '\''))) {
		msg[len-1] = 0;
		msg++;
	}
	fprintf(stderr, "%s\n", msg);
	free(eval_cmdline);
	return 0;
}

int parser_include_rules(struct tupfile *tf, const char *tuprules)
{
	int trlen = strlen(tuprules);
	int rc = -1;
	struct stat buf;
	int num_dotdots;
	struct tup_entry *tent;
	char *path;
	char *p;
	int x;
	const char dotdotstr[] = {'.', '.', path_sep(), 0};

	if(tf->including_rules) {
		fprintf(tf->f, "tup error: Unable to call include_rules from within a Tuprules.tup context.\n");
		return SYNTAX_ERROR;
	}
	tf->including_rules = 1;
	num_dotdots = 0;
	tent = variant_tent_to_srctent(tf->curtent);
	while(tent->tnode.tupid != DOT_DT) {
		tent = tent->parent;
		num_dotdots++;
	}
	path = malloc(num_dotdots * 3 + trlen + 1);
	if(!path) {
		parser_error(tf, "malloc");
		return -1;
	}

	p = path;
	for(x=0; x<num_dotdots; x++, p += 3) {
		strcpy(p, dotdotstr);
	}
	strcpy(path + num_dotdots*3, tuprules);

	p = path;
	for(x=0; x<=num_dotdots; x++, p += 3) {
		if(handle_file_dtent(ACCESS_READ, tf->curtent, p, &tf->ps->s.finfo) < 0) {
			free(path);
			return -1;
		}
		if(fstatat(tf->cur_dfd, p, &buf, AT_SYMLINK_NOFOLLOW) == 0)
			if(parser_include_file(tf, p) < 0)
				goto out_free;
	}
	rc = 0;

out_free:
	free(path);

	tf->including_rules = 0;
	return rc;
}

struct readdir_parser_params {
	struct string_entries *root;
};

static int readdir_parser_cb(void *arg, struct tup_entry *tent)
{
	struct readdir_parser_params *rpp = arg;
	struct string_tree *st;

	if(is_virtual_tent(tent))
		return 0;

	st = malloc(sizeof *st);
	if(string_tree_add(rpp->root, st, tent->name.s) < 0) {
		/* string_tree_add will fail if there is a dup. Just
		 * free our st and return.
		 */
		free(st);
	}
	return 0;
}

static void free_dir_list(struct string_entries *root, struct parser_directory *pd)
{
	free_string_tree(&pd->files);
	string_tree_remove(root, &pd->st);
	free(pd);
}

static void free_dir_lists(struct string_entries *root)
{
	struct string_tree *st;

	while((st = RB_ROOT(root)) != NULL) {
		struct parser_directory *pd = container_of(st, struct parser_directory, st);

		free_dir_list(root, pd);
	}
}

static int gen_dir_list(struct tupfile *tf, tupid_t dt)
{
	char path[PATH_MAX];
	struct tup_entry *tent;
	struct tup_entry *srctent;
	struct parser_directory *pd;
	struct readdir_parser_params rpp;
	struct string_tree *st;

	if(tup_entry_add(dt, &tent) < 0)
		return -1;
	pd = malloc(sizeof *pd);
	if(!pd) {
		perror("malloc");
		return -1;
	}
	RB_INIT(&pd->files);

	rpp.root = &pd->files;

	if(snprint_tup_entry(path, sizeof(path), variant_tent_to_srctent(tent)) >= (signed)sizeof(path)) {
		fprintf(tf->f, "tup internal error: ps.path is sized incorrectly in gen_dir_list()\n");
		return -1;
	}
	if(tup_db_select_node_dir_glob(readdir_parser_cb, &rpp, tent,
				       "*", -1, &tf->g->gen_delete_root, 1) < 0)
		return -EIO;
	if(variant_get_srctent(tf->variant, tent, &srctent) < 0)
		return -1;
	if(srctent) {
		if(tup_db_select_node_dir_glob(readdir_parser_cb, &rpp, srctent,
					       "*", -1, &tf->g->gen_delete_root, 1) < 0)
			return -EIO;
	}

	st = string_tree_search(&tf->ps->directories, path, strlen(path));
	if(st)
		free_dir_list(&tf->ps->directories, container_of(st, struct parser_directory, st));

	if(string_tree_add(&tf->ps->directories, &pd->st, path) < 0) {
		fprintf(tf->f, "tup internal error: Unable to add '%s' to the directories string tree\n", path);
		return -1;
	}
	return 0;
}

static int preload(struct tupfile *tf, char *cmdline)
{
	struct path_list_head plist;
	struct path_list *pl;

	TAILQ_INIT(&plist);
	if(get_path_list(tf, cmdline, &plist, NULL) < 0)
		return -1;
	if(eval_path_list(tf, &plist, NULL, EXPAND_NODES) < 0)
		return -1;

	/* get_path_list() leaves us with the last path uncompleted (since it
	 * usually just handles filenames), so we resolve the last path here
	 * and store that in dt.  We don't need the pel for
	 * parse_dependent_tupfiles().
	 */
	TAILQ_FOREACH(pl, &plist, list) {
		struct tup_entry *tent;

		if(path_list_fill_dt_pel(tf, pl, tf->curtent->tnode.tupid, 0) < 0)
			return -1;
		if(tup_entry_add(pl->dt, &tent) < 0)
			return -1;
		if(pl->pel->len == 2 && strncmp(pl->pel->path, "..", 2) == 0) {
			if(!tent->parent) {
				fprintf(tf->f, "tup error: Unable to preload a directory beyond the tup hierarchy.\n");
				return -1;
			}
			tent = tent->parent;
		} else {
			if(tup_db_select_tent_part(tent, pl->pel->path, pl->pel->len, &tent) < 0)
				return -1;
			if(!tent) {
				fprintf(tf->f, "tup error: Unable to find node '%.*s' for preloading in directory %lli\n", pl->pel->len, pl->pel->path, pl->dt);
				tup_db_print(tf->f, pl->dt);
				return -1;
			}
		}
		if(tent->type != TUP_NODE_DIR) {
			fprintf(tf->f, "tup error: preload needs to specify a pathname, but node '%.*s' has type '%s'\n", pl->pel->len, pl->pel->path, tup_db_type(tent->type));
			return -1;
		}
		pl->dt = tent->tnode.tupid;
	}
	if(parse_dependent_tupfiles(&plist, tf) < 0)
		return -1;

	TAILQ_FOREACH(pl, &plist, list) {
		if(gen_dir_list(tf, pl->dt) < 0)
			return -1;
	}
	free_path_list(&plist);
	return 0;
}

static int run_script(struct tupfile *tf, char *cmdline, int lno)
{
	int rc;
	char *eval_cmdline;

	eval_cmdline = eval(tf, cmdline, EXPAND_NODES);
	if(!eval_cmdline) {
		return -1;
	}
	rc = exec_run_script(tf, eval_cmdline, lno);
	free(eval_cmdline);
	return rc;
}

int exec_run_script(struct tupfile *tf, const char *cmdline, int lno)
{
	char *rules;
	char *p;
	int rslno = 0;
	int rc;
	struct tent_tree *tt;

	pthread_mutex_lock(&tf->ps->lock);
	rc = gen_dir_list(tf, tf->tent->tnode.tupid);
	pthread_mutex_unlock(&tf->ps->lock);
	if(rc < 0)
		return -1;

	if(debug_run)
		fprintf(tf->f, " --- run script output from '%s'\n", cmdline);

	/* Make sure we have a dependency on each environment variable, since
	 * these are passed to the run script.
	 */
	RB_FOREACH(tt, tent_entries, &tf->env_root) {
		if(tent_tree_add_dup(&tf->input_root, tt->tent) < 0)
			return -1;
	}
	if (tf->use_server)
		rc = server_run_script(tf->f, tf->tent->tnode.tupid, cmdline, &tf->env_root, &rules);
	else
		rc = serverless_run_script(tf->f, cmdline, &tf->env_root, &rules);
	if(rc < 0)
		return -1;

	p = rules;
	while(p[0]) {
		char *newline;
		rslno++;
		newline = strchr(p, '\n');
		if(!newline) {
			fprintf(tf->f, "tup error: Missing newline from :-rule in run script: '%s'\n", p);
			goto out_err;
		}
		*newline = 0;
		if(debug_run)
			fprintf(tf->f, "%s\n", p);
		if(p[0] == ':') {
			if(parse_rule(tf, p+1, lno) < 0) {
				fprintf(tf->f, "tup error: Unable to parse :-rule from run script: '%s'\n", p);
				goto out_err;
			}
		} else if(p[0] == '#' || p[0] == 0) {
			/* Skip comments and blank lines */
			if(debug_run) {
				fprintf(tf->f, "Skipping non :-rule line %i: %s\n", rslno, p);
			}
		} else {
			fprintf(tf->f, "tup error: run-script line %i is not a :-rule - '%s'\n", rslno, p);
			goto out_err;
		}
		p = newline + 1;
	}

	free(rules);
	return 0;

out_err:
	free(rules);
	return -1;
}

static char *capture_run_script_output(struct tupfile *tf, const char *cmdline)
{
	char *output;
	int rc;
	struct tent_tree *tt;

	pthread_mutex_lock(&tf->ps->lock);
	rc = gen_dir_list(tf, tf->tent->tnode.tupid);
	pthread_mutex_unlock(&tf->ps->lock);
	if(rc < 0)
		return NULL;

	RB_FOREACH(tt, tent_entries, &tf->env_root) {
		if(tent_tree_add_dup(&tf->input_root, tt->tent) < 0)
			return NULL;
	}

	if(tf->use_server)
		rc = server_run_script(tf->f, tf->tent->tnode.tupid, cmdline, &tf->env_root, &output);
	else
		rc = serverless_run_script(tf->f, cmdline, &tf->env_root, &output);
	if(rc < 0)
		return NULL;
	return output;
}

static char *eval_backticks(struct tupfile *tf, const char *string)
{
	const char *s = string;
	struct estring e;
	size_t x;

	if(strchr(string, '`') == NULL)
		return strdup(string);

	if(estring_init(&e) < 0)
		return NULL;
	while(*s) {
		const char *tick;
		char *cmd;
		char *output;
		size_t len;

		tick = strchr(s, '`');
		if(!tick) {
			if(estring_append(&e, s, strlen(s)) < 0)
				goto err;
			break;
		}
		if(estring_append(&e, s, tick-s) < 0)
			goto err;
		s = tick + 1;
		tick = strchr(s, '`');
		if(!tick) {
			fprintf(tf->f, "tup error: Missing closing backtick in path expression '%s'.\n", string);
			goto err;
		}
		len = tick - s;
		cmd = malloc(len + 1);
		if(!cmd) {
			perror("malloc");
			goto err;
		}
		memcpy(cmd, s, len);
		cmd[len] = 0;
		output = capture_run_script_output(tf, cmd);
		free(cmd);
		if(!output)
			goto err;
		len = strlen(output);
		while(len > 0 && (output[len-1] == '\n' || output[len-1] == '\r'))
			len--;
		for(x = 0; x < len; x++) {
			if(output[x] == '\n' || output[x] == '\r')
				output[x] = ' ';
		}
		if(estring_append(&e, output, len) < 0) {
			free(output);
			goto err;
		}
		free(output);
		s = tick + 1;
	}
	return e.s;

err:
	free(e.s);
	return NULL;
}

int export(struct tupfile *tf, const char *cmdline)
{
	struct var_entry *ve = NULL;

	if(!cmdline[0]) {
		fprintf(tf->f, "tup error: Expected environment variable to export.\n");
		return SYNTAX_ERROR;
	}

	/* Pull from tup's environment */
	if(tup_db_findenv(cmdline, -1, &ve) < 0) {
		fprintf(tf->f, "tup error: Unable to get tup entry for environment variable '%s'\n", cmdline);
		return -1;
	}
	if(tent_tree_add_dup(&tf->env_root, ve->tent) < 0)
		return -1;
	return 0;
}

int import(struct tupfile *tf, const char *cmdline, const char **retvar, const char **retval)
{
	struct var_entry *ve = NULL;
	const char *var;
	const char *default_val = NULL;
	int varlen = 0;

	if(!cmdline[0]) {
		fprintf(tf->f, "tup error: Expected environment variable to import.\n");
		return SYNTAX_ERROR;
	}

	var = cmdline;
	const char *p = cmdline;
	while(*p) {
		if(*p == '=') {
			default_val = p + 1;
			break;
		}
		varlen++;
		p++;
	}

	/* Pull from tup's environment */
	if(tup_db_findenv(var, varlen, &ve) < 0) {
		fprintf(tf->f, "tup error: Unable to get tup entry for environment variable '%.*s'\n", varlen, var);
		return -1;
	}
	if(tent_tree_add_dup(&tf->input_root, ve->tent) < 0)
		return -1;
	/* Values in envdb are stored as VAR=value */
	const char *real_val = ve->value;
	if(real_val) {
		while(*real_val) {
			real_val++;
			if(*real_val == '=') {
				real_val++;
				break;
			}
		}
	} else {
		real_val = default_val;
	}
	if(luadb_set(ve->var.s, real_val) < 0)
		return -1;
	if(retvar) {
		*retvar = ve->var.s;
	}
	if(retval) {
		*retval = real_val;
	}
	return 0;
}

/* If a .gitignore directive is removed, we need to either revert back to the
 * user's explicit .gitignore file, or remove it entirely.
 */
int remove_tup_gitignore(FILE *err, struct graph *g, struct tup_entry *tent)
{
	int dfd;
	int fdold;
	int fdnew;
	const char *tg_str = "##### TUP GITIGNORE #####\n";
	int tg_str_len = 26;
	int tg_idx = 0;
	int copied_tg_str = 0;
	int bytes_copied = 0;

	dfd = tup_entry_open(tent->parent);
	if(dfd < 0)
		return -1;

	fdold = openat(dfd, ".gitignore", O_RDONLY);
	if(fdold < 0) {
		perror(".gitignore");
		fprintf(err, "tup error: Unable to open gitignore file in directory: ");
		print_tup_entry(err, tent->parent);
		fprintf(err, "\n");
		return -1;
	}
	fdnew = openat(dfd, ".gitignore.new", O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if(fdnew < 0) {
		perror(".gitignore.new");
		fprintf(err, "tup error: Unable to create new gitignore file in directory: ");
		print_tup_entry(err, tent->parent);
		fprintf(err, "\n");
		return -1;
	}
	while(1) {
		char nextchar;
		if(read(fdold, &nextchar, 1) < 1) {
			break;
		}
		if(tg_str[tg_idx] == nextchar) {
			tg_idx++;
		} else {
			tg_idx = 0;
		}
		if(write(fdnew, &nextchar, 1) != 1) {
			perror("write");
			return -1;
		}
		bytes_copied++;
		if(tg_idx == tg_str_len) {
			copied_tg_str = 1;
			break;
		}
	}
	if(copied_tg_str) {
		bytes_copied -= tg_str_len;
		if(ftruncate(fdnew, bytes_copied) < 0) {
			perror("ftruncate");
			fprintf(err, "tup error: Unable to truncate .gitignore.new file\n");
			return -1;
		}
	}
	if(close(fdnew) < 0) {
		perror("close(fdnew)");
		return -1;
	}
	if(close(fdold) < 0) {
		perror("close(fdold)");
		return -1;
	}
	if(bytes_copied) {
		if(renameat(dfd, ".gitignore.new", dfd, ".gitignore") < 0) {
			perror("renameat");
			fprintf(err, "tup error: Unable to move .gitignore.new file over .gitignore");
			return -1;
		}
		if(tup_db_set_type(tent, TUP_NODE_FILE) < 0)
			return -1;
		if(tup_db_set_srcid(tent, -1) < 0)
			return -1;
		tent_tree_remove(&g->gen_delete_root, tent);
		tent_tree_remove(&g->save_root, tent);
	} else {
		if(unlinkat(dfd, ".gitignore.new", 0) < 0) {
			perror("unlinkat");
			fprintf(err, "tup error: Unable to unlink .gitignore.new file in directory: ");
			print_tup_entry(err, tent->parent);
			fprintf(err, "\n");
			return -1;
		}
		/* The .gitignore tent is in the delete root, so the updater
		 * will clean it up.
		 */
	}
	if(close(dfd) < 0) {
		perror("close(dfd)");
		return -1;
	}
	return 0;
}

static int gitignore(struct tupfile *tf, struct tup_entry *dtent)
{
	struct tup_entry *tent;

	if(tup_db_select_tent(dtent, ".gitignore", &tent) < 0)
		return -1;
	if(!tent) {
		if(tf->refactoring) {
			fprintf(tf->f, "tup refactoring error: Attempting to create a new .gitignore file.\n");
			return -1;
		}
		if(tup_db_node_insert_tent(dtent, ".gitignore", -1, TUP_NODE_GENERATED, INVALID_MTIME, dtent->tnode.tupid, &tent) < 0)
			return -1;
	} else {
		tent_tree_remove(&tf->g->gen_delete_root, tent);
		tent_tree_remove(&tf->g->save_root, tent);
		/* It may be a ghost if we are going from a variant
		 * to an in-tree build, or a normal file if we are appending
		 * definitions to a user-created .gitignore file.
		 */
		if(tent->type != TUP_NODE_GENERATED) {
			if(tup_db_set_type(tent, TUP_NODE_GENERATED) < 0)
				return -1;
			if(tup_db_set_srcid(tent, dtent->tnode.tupid) < 0)
				return -1;
		}
	}
	if(tent_tree_add_dup(&tf->g->parse_gitignore_root, dtent) < 0)
		return -1;
	return 0;
}

static int check_toplevel_gitignore(struct tupfile *tf)
{
	int fd;
	struct buf incb;
	struct tup_entry *tent = NULL;
	char *p;
	char *e;
	char *line;
	int rc = -1;

	/* This mimics pieces of include_tuprules and parse_tupfile(), but all
	 * we're looking for is a .gitignore directive. We don't want to
	 * actually do include_rules(), since we'll potentially pick up real
	 * :-rules from Tuprules.tup, which would be confusing since there is
	 * no actual Tupfile. This isn't perfect, sicne it wouldn't handle a
	 * .gitignore directive inside a conditional, but that doesn't really
	 * make sense anyway.
	 */
	if(tup_db_select_tent(tf->tent, "Tuprules.tup", &tent) < 0)
		return -1;
	if(!tent || tent->type == TUP_NODE_GHOST)
		return 0;
	fd = parser_entry_open(tf, tent);
	if(fd < 0) {
		if(errno == ENOENT)
			return 0;
		parser_error(tf, "Tuprules.tup");
		fprintf(tf->f, "tup error: Unable to open top-level Tuprules.tup to check for .gitignore directive.\n");
		return -1;
	}
	if(fslurp_null(fd, &incb) < 0)
		goto out_close;

	p = incb.s;
	e = incb.s + incb.len;
	while(p < e) {
		char *newline;

		/* Skip leading whitespace and empty lines */
		while(p < e && isspace(*p)) {
			p++;
		}
		/* If we just had empty lines at the end, we're done */
		if(p == e)
			break;

		line = p;
		newline = get_newline(p);
		if(!newline) {
			fprintf(tf->f, "tup error: Unable to find trailing nul-byte.\n");
			fprintf(tf->f, "  Line was: '%s'\n", line);
			goto out_free;
		}
		while(newline[-1] == '\\' || (newline[-2] == '\\' && newline[-1] == '\r')) {
			if (newline[-1] == '\r') {
				newline[-2] = ' ';
			}
			newline[-1] = ' ';
			newline[0] = ' ';
			newline = get_newline(p);
			if(!newline) {
				fprintf(tf->f, "tup error: Unable to find trailing nul-byte.\n");
				fprintf(tf->f, "  Line was: '%s'\n", line);
				goto out_free;
			}
		}

		p = newline + 1;

		/* Remove trailing whitespace */
		while(newline > line && isspace(newline[-1])) {
			newline--;
		}
		*newline = 0;

		if(line[0] == '#') {
			/* Skip comments */
			continue;
		}

		if(strcmp(line, ".gitignore") == 0) {
			tf->ign = 1;
			break;
		}
	}
	rc = 0;

out_free:
	free(incb.s);
out_close:
	if(close(fd) < 0) {
		parser_error(tf, "close(fd)");
	}
	return rc;
}

int parser_include_file(struct tupfile *tf, const char *file)
{
	struct buf incb;
	int fd;
	int rc = -1;
	struct pel_group pg;
	struct path_element *pel = NULL;
	struct tup_entry *tent = NULL;
	tupid_t newdt;
	struct tup_entry *oldtent = tf->curtent;
	int old_dfd = tf->cur_dfd;
	struct tup_function_registry *old_registry = tf->function_registry;
	struct tup_entry *srctent = NULL;
	struct tup_entry *newtent;
	const char *lua;

	if(get_path_elements(file, &pg) < 0)
		goto out_err;
	if(pg.pg_flags & PG_HIDDEN) {
		fprintf(tf->f, "tup error: Unable to include file with hidden path element.\n");
		goto out_del_pg;
	}
	newdt = find_dir_tupid_dt_pg(tf->curtent->tnode.tupid, &pg, &pel, SOTGV_NO_GHOST, 0);
	if(newdt <= 0) {
		fprintf(tf->f, "tup error: Unable to find directory for include file '%s' relative to '", file);
		print_tup_entry(tf->f, tf->curtent);
		fprintf(tf->f, "'\n");
		goto out_del_pg;
	}
	if(!pel) {
		fprintf(tf->f, "tup error: Invalid include filename: '%s'\n", file);
		goto out_del_pg;
	}

	newtent = tup_entry_get(newdt);
	if(tup_entry_variant(newtent) != tf->variant) {
		fprintf(tf->f, "tup error: Unable to include file '%s' since it is outside of the variant tree.\n", file);
		return -1;
	}
	tf->curtent = newtent;

	if(variant_get_srctent(tf->variant, newtent, &srctent) < 0)
		return -1;
	if(!srctent)
		srctent = tf->curtent;
	if(tup_db_select_tent_part(srctent, pel->path, pel->len, &tent) < 0 || !tent) {
		fprintf(tf->f, "tup error: Unable to find tup entry for file '%s'\n", file);
		goto out_free_pel;
	}

	tf->cur_dfd = tup_entry_openat(tf->root_fd, tent->parent);
	if(tf->cur_dfd < 0) {
		parser_error(tf, file);
		goto out_free_pel;
	}
	fd = parser_entry_open(tf, tent);
	if(fd < 0) {
		goto out_close_dfd;
	}
	if(fslurp_null(fd, &incb) < 0)
		goto out_close;

	lua = strstr(file, ".lua");
	/* strcmp is to make sure .lua is at the end of the filename */
	if(lua && strcmp(lua, ".lua") == 0) {
		if(parse_lua_tupfile(tf, &incb, file) < 0)
			goto out_free;
	} else {
		tf->function_registry = malloc(sizeof *tf->function_registry);
		if(!tf->function_registry) {
			perror("malloc");
			goto out_free;
		}
		function_registry_init(tf->function_registry);
		if(parse_tupfile(tf, &incb, file) < 0)
			goto out_free;
	}
	rc = 0;
out_free:
	if(tf->function_registry != old_registry) {
		free_function_registry(tf->function_registry);
		tf->function_registry = old_registry;
	}
	free(incb.s);
out_close:
	if(close(fd) < 0) {
		parser_error(tf, "close(fd)");
		rc = -1;
	}
out_close_dfd:
	if(close(tf->cur_dfd) < 0) {
		parser_error(tf, "close(tf->cur_dfd)");
		rc = -1;
	}
out_free_pel:
	free_pel(pel);
out_del_pg:
	del_pel_group(&pg);

out_err:
	tf->curtent = oldtent;
	tf->cur_dfd = old_dfd;
	if(rc < 0) {
		fprintf(tf->f, "tup error: Failed to parse included file '%s'\n", file);
		return -1;
	}

	return 0;
}

void init_rule(struct rule *r)
{
	r->foreach = 0;
	r->bin = NULL;
	r->command = NULL;
	r->extra_command = NULL;
	r->command_len = 0;
	init_name_list(&r->inputs);
	init_name_list(&r->order_only_inputs);
	init_name_list(&r->bang_oo_inputs);
	TAILQ_INIT(&r->order_only_input_paths);
	TAILQ_INIT(&r->outputs);
	TAILQ_INIT(&r->extra_outputs);
	TAILQ_INIT(&r->bang_extra_outputs);
	r->empty_input = 0;
	r->line_number = -1;
}

static int parse_rule(struct tupfile *tf, char *p, int lno)
{
	char *input, *cmd, *output, *bin;
	int cmd_len;
	struct rule r;
	struct name_list output_nl;
	int is_variant_copy = 0;
	int rc;

	init_rule(&r);

	if(split_input_pattern(tf, p, &input, &cmd, &cmd_len, &output, &bin) < 0)
		return -1;
	if(bin) {
		if((r.bin = bin_add(bin, current_bin_head(tf))) == NULL)
			return -1;
	} else {
		r.bin = NULL;
	}

	r.foreach = 0;
	if(input) {
		if(strncmp(input, "foreach", 7) == 0) {
			r.foreach = 1;
			input += 7;
			while(isspace(*input)) input++;
		}
	}

	/* Make sure we have an input string, and the input string doesn't
	 * just contain order-only inputs.
	 */
	if(input && input[0] != '|') {
		r.empty_input = 0;
	} else {
		r.empty_input = 1;
	}
	if(strcmp(cmd, "!tup_preserve") == 0)
		is_variant_copy = 1;
	if(parse_input_pattern(tf, input, &r.inputs, &r.order_only_input_paths, current_bin_head(tf), is_variant_copy) < 0)
		return -1;

	r.command = cmd;
	r.command_len = cmd_len;
	r.line_number = lno;

	if(parse_output_pattern(tf, output, &r.outputs, &r.extra_outputs) < 0)
		return -1;

	if(r.command[0] == '!') {
		char *space;
		space = memchr(cmd, ' ', r.command_len);
		if(space) {
			*space = 0;
			r.extra_command = space + 1;
			r.command_len = strlen(r.command);
		}
	}

	init_name_list(&output_nl);
	rc = execute_rule(tf, &r, &output_nl);
	if(rc == 0 && tf->func_frame && tf->func_frame->build_ctx) {
		if(move_name_list(&tf->func_frame->build_ctx->outputs, &output_nl) < 0)
			rc = -1;
	}
	delete_name_list(&output_nl);
	free_path_list(&r.order_only_input_paths);
	free_path_list(&r.outputs);
	free_path_list(&r.extra_outputs);
	free_path_list(&r.bang_extra_outputs);
	return rc;
}

static char *split_eq(char *p)
{
	char *eq;
	char *value;

	eq = strchr(p, '=');
	if(!eq) {
		return NULL;
	}
	value = eq + 1;
	while(*value && isspace(*value))
		value++;
	eq--;
	while(isspace(*eq))
		eq--;
	eq[1] = 0;
	return value;
}

static struct bang_rule *alloc_br(void)
{
	struct bang_rule *br;
	br = malloc(sizeof *br);
	if(!br) {
		perror("malloc");
		return NULL;
	}
	br->value = NULL;
	br->input = NULL;
	br->command = NULL;
	TAILQ_INIT(&br->outputs);
	TAILQ_INIT(&br->extra_outputs);
	return br;
}

static struct string_tree *find_bang_rule(struct tupfile *tf, const char *name, int len)
{
	struct string_tree *st = NULL;

	if(tf->func_frame)
		st = string_tree_search(&tf->func_frame->bang_root, name, len);
	if(!st)
		st = string_tree_search(&tf->bang_root, name, len);
	return st;
}

static int parse_bang_definition(struct tupfile *tf, char *p, int lno)
{
	struct string_tree *st;
	struct string_entries *bang_root = tf->in_rules_block && tf->func_frame ? &tf->func_frame->bang_root : &tf->bang_root;
	char *input;
	char *command;
	int command_len;
	int foreach = 0;
	char *output;
	char *value;
	char *alloc_value;
	char *bin;
	struct bang_rule *br;

	value = split_eq(p);
	if(!value) {
		fprintf(tf->f, "tup error: Parse error line %i: Expecting '=' to set the bang rule.\n", lno);
		return SYNTAX_ERROR;
	}

	if(value[0] == '!') {
		/* Alias one macro as another */
		struct bang_rule *cur_br;

		st = string_tree_search(bang_root, p, strlen(p));
		if(st) {
			free_bang_rule(bang_root,
				       container_of(st, struct bang_rule, st));
		}

		st = find_bang_rule(tf, value, strlen(value));
		if(!st) {
			fprintf(tf->f, "tup error: Unable to find !-macro '%s'\n", value);
			return -1;
		}
		cur_br = container_of(st, struct bang_rule, st);

		br = alloc_br();
		if(!br) {
			return -1;
		}
		br->foreach = cur_br->foreach;
		br->value = NULL;
		if(cur_br->input) {
			br->input = strdup(cur_br->input);
			if(!br->input) {
				parser_error(tf, "strdup");
				goto err_cleanup_br;
			}
		} else {
			br->input = NULL;
		}
		br->command = strdup(cur_br->command);
		if(!br->command) {
			parser_error(tf, "strdup");
			goto err_cleanup_br;
		}
		if(copy_path_list(tf, &br->outputs, &cur_br->outputs) < 0)
			goto err_cleanup_br;
		if(copy_path_list(tf, &br->extra_outputs, &cur_br->extra_outputs) < 0)
			goto err_cleanup_br;

		br->command_len = cur_br->command_len;

		if(string_tree_add(bang_root, &br->st, p) < 0) {
			fprintf(tf->f, "tup internal error: Error inserting bang rule into tree\n");
			goto err_cleanup_br;
		}
		return 0;
	}

	alloc_value = strdup(value);
	if(!alloc_value) {
		parser_error(tf, "strdup");
		return -1;
	}

	if(split_input_pattern(tf, alloc_value, &input, &command, &command_len,
			       &output, &bin) < 0)
		return -1;
	if(bin != NULL) {
		fprintf(tf->f, "tup error: bins aren't allowed in !-macros. Rule was: %s = %s\n", p, alloc_value);
		return -1;
	}

	if(input) {
		if(strncmp(input, "foreach", 7) == 0) {
			foreach = 1;
			input += 7;
			while(isspace(*input)) input++;
		}
		if(input[0]) {
			if(input[0] != '|') {
				fprintf(tf->f, "tup error: !-macros can't have normal inputs, only order-only inputs. Pattern was: %s\n", input);
				return -1;
			}
			input++;
			while(isspace(*input)) input++;
		}
	}

	st = string_tree_search(bang_root, p, strlen(p));
	if(st) {
		/* Replace existing !-macro */
		struct bang_rule *cur_br;

		cur_br = container_of(st, struct bang_rule, st);
		free(cur_br->value);
		cur_br->foreach = foreach;
		cur_br->value = alloc_value;
		cur_br->input = input;
		cur_br->command = command;
		cur_br->command_len = command_len;
		free_path_list(&cur_br->outputs);
		free_path_list(&cur_br->extra_outputs);
		if(parse_output_pattern(tf, output, &cur_br->outputs, &cur_br->extra_outputs) < 0)
			return -1;
	} else {
		/* Create new !-macro */
		br = alloc_br();
		if(!br) {
			return -1;
		}
		br->foreach = foreach;
		br->input = input;
		br->command = command;
		br->command_len = command_len;
		if(parse_output_pattern(tf, output, &br->outputs, &br->extra_outputs) < 0)
			goto err_cleanup_br;
		br->value = alloc_value;

		if(string_tree_add(bang_root, &br->st, p) < 0) {
			fprintf(tf->f, "tup internal error: Error inserting bang rule into tree\n");
			goto err_cleanup_br;
		}
	}
	return 0;

err_cleanup_br:
	free(br->command);
	free(br->input);
	free(br->value);
	free(br);
	return -1;
}

static int set_variable(struct tupfile *tf, char *line)
{
	char *eq;
	char *var;
	char *value;
	int append;
	int rc = 0;

	/* Find the += or = sign, and point value to the start
	 * of the string after that op.
	 */
	eq = strstr(line, "+=");
	if(eq) {
		value = eq + 2;
		append = 1;
	} else {
		eq = strstr(line, ":=");
		if(eq) {
			value = eq + 2;
			append = 0;
		} else {
			eq = strchr(line, '=');
			if(!eq)
				return SYNTAX_ERROR;
			value = eq + 1;
			append = 0;
		}
	}

	/* End the lval with a 0, then space-delete the end
	 * of the variable name and the beginning of the value.
	 */
	*eq = 0;
	while(isspace(*value) && *value != 0)
		value++;
	eq--;
	while(isspace(*eq) && eq > line) {
		*eq = 0;
		eq--;
	}

	var = eval(tf, line, KEEP_NODES);
	if(!var)
		return -1;
	value = eval(tf, value, KEEP_NODES);
	if(!value)
		return -1;

	if(strncmp(var, "CONFIG_", 7) == 0) {
		fprintf(tf->f, "tup error: Unable to override setting of variable '%s' because it begins with 'CONFIG_'. These variables can only be set in the tup.config file.\n", var);
		return -1;
	}

	if(var[0] == '&') {
		struct tup_entry *tent;
		tent = get_tent_dt(variant_tent_to_srctent(tf->curtent)->tnode.tupid, value);
		if(!tent || tent->type == TUP_NODE_GHOST) {
			fprintf(tf->f, "tup error: Unable to find tup entry for file '%s' in node reference declaration.\n", value);
			return -1;
		}

		if(tent->type != TUP_NODE_FILE && tent->type != TUP_NODE_DIR) {
			fprintf(tf->f, "tup error: Node-variables can only refer to normal files and directories, not a '%s'.\n", tup_db_type(tent->type));
			return -1;
		}
		if(tent_tree_add_dup(&tf->input_root, tent) < 0)
			return -1;

		free(value);
		value = malloc(32);
		if(!value) {
			perror("malloc");
			return -1;
		}
		snprintf(value, 31, "%%%llit", tent->tnode.tupid);
		value[31] = 0;

		/* var+1 to skip the leading '&' */
		if(append)
			rc = vardb_append(&tf->node_db, var+1, value);
		else
			rc = vardb_set(&tf->node_db, var+1, value, NULL);
	} else {
		if(tf->func_frame) {
			if(append)
				rc = vardb_append(&tf->func_frame->vars, var, value);
			else
				rc = vardb_set(&tf->func_frame->vars, var, value, NULL);
		} else {
			if(append)
				rc = luadb_append(var, value);
			else
				rc = luadb_set(var, value);
		}
	}
	if(rc < 0) {
		fprintf(tf->f, "tup internal error: Error setting variable '%s'\n", var);
		return -1;
	}
	free(var);
	free(value);
	return 0;
}

static int parse_bind(struct tupfile *tf, char *line)
{
	char *eq;
	char *var;
	char *key;
	char *fallback = NULL;
	char *keyeval;
	char *value = NULL;
	struct var_entry *ve;

	eq = strstr(line, ":=");
	if(!eq)
		eq = strchr(line, '=');
	if(!eq)
		return SYNTAX_ERROR;
	*eq = 0;
	var = line;
	while(isspace(*var))
		var++;
	key = eq + (eq[1] == '=' ? 2 : 1);
	while(isspace(*key))
		key++;
	eq--;
	while(eq >= var && isspace(*eq)) {
		*eq = 0;
		eq--;
	}
	if(var[0] == 0)
		return SYNTAX_ERROR;

	fallback = strstr(key, "||");
	if(fallback) {
		*fallback = 0;
		fallback += 2;
		while(isspace(*fallback))
			fallback++;
	}
	while(*key && isspace(key[strlen(key)-1]))
		key[strlen(key)-1] = 0;
	if(key[0] != '"' || key[strlen(key)-1] != '"')
		return SYNTAX_ERROR;
	key[strlen(key)-1] = 0;
	keyeval = key + 1;

	if(tf->func_frame)
		ve = vardb_get(&tf->func_frame->args, keyeval, strlen(keyeval));
	else
		ve = NULL;
	if(ve && ve->value) {
		value = strdup(ve->value);
		if(!value) {
			parser_error(tf, "strdup");
			return -1;
		}
	} else if(fallback) {
		value = eval(tf, fallback, KEEP_NODES);
		if(!value)
			return -1;
	} else {
		value = strdup("");
		if(!value) {
			parser_error(tf, "strdup");
			return -1;
		}
	}

	if(tf->func_frame) {
		if(vardb_set(&tf->func_frame->vars, var, value, NULL) < 0)
			return -1;
	} else {
		if(luadb_set(var, value) < 0)
			return -1;
	}
	free(value);
	return 0;
}

static int parse_return_stmt(struct tupfile *tf, char *line)
{
	struct vardb returns;
	int rc;

	rc = parse_arg_map(tf, line, &returns, 0);
	if(rc < 0)
		return rc;
	vardb_close(&tf->func_frame->returns);
	tf->func_frame->returns = returns;
	tf->func_frame->has_return = 1;
	return RETURN_DIRECTIVE;
}

static int set_function_or_lua_var(struct tupfile *tf, const char *var, const char *value)
{
	if(tf->func_frame)
		return vardb_set(&tf->func_frame->vars, var, value, NULL);
	return luadb_set(var, value);
}

static int assign_fbind_vars(struct tupfile *tf, struct vardb *bindings, struct vardb *returns)
{
	struct string_tree *st;

	RB_FOREACH(st, string_entries, &bindings->root) {
		struct var_entry *binding = container_of(st, struct var_entry, var);
		struct var_entry *ret;

		ret = vardb_get(returns, binding->var.s, binding->var.len);
		if(!ret || !ret->value) {
			fprintf(tf->f, "tup error: fbind expected returned key '%s'.\n", binding->var.s);
			return -1;
		}
		if(set_function_or_lua_var(tf, binding->value, ret->value) < 0)
			return -1;
	}
	return 0;
}

static int parse_simple_fbind_var(const char *text, char **out)
{
	char *var;

	var = dup_trimmed_range(text, text + strlen(text));
	if(!var) {
		perror("strndup");
		return -1;
	}
	if(var[0] == 0 || strchr(var, '{') || strchr(var, '"') || strchr(var, '}') ||
	   strchr(var, ',') || isspace((unsigned char)var[0]) ||
	   strpbrk(var, " \t\r\n") != NULL) {
		free(var);
		return SYNTAX_ERROR;
	}
	*out = var;
	return 0;
}

static int parse_fbind_map(const char *text, struct vardb *bindings)
{
	const char *s = text;

	if(vardb_init(bindings) < 0)
		return -1;
	while(isspace(*s))
		s++;
	if(*s != '{')
		return SYNTAX_ERROR;
	s++;
	for(;;) {
		char *key = NULL;
		char *var = NULL;
		const char *start;
		const char *end;
		int len;

		while(isspace(*s) || *s == ',')
			s++;
		if(*s == '}')
			break;
		if(parse_quoted_string(&s, &key) < 0)
			return SYNTAX_ERROR;
		while(isspace(*s))
			s++;
		if(*s != ':') {
			free(key);
			return SYNTAX_ERROR;
		}
		s++;
		while(isspace(*s))
			s++;
		start = s;
		while(*s && *s != ',' && *s != '}')
			s++;
		end = s;
		while(end > start && isspace(end[-1]))
			end--;
		len = end - start;
		if(len <= 0) {
			free(key);
			return SYNTAX_ERROR;
		}
		var = strndup(start, len);
		if(!var) {
			perror("strndup");
			free(key);
			return -1;
		}
		if(vardb_set(bindings, key, var, NULL) < 0) {
			free(key);
			free(var);
			return -1;
		}
		free(key);
		free(var);
		while(isspace(*s))
			s++;
		if(*s == '}')
			break;
		if(*s != ',')
			return SYNTAX_ERROR;
	}
	while(isspace(*s))
		s++;
	if(*s != '}')
		return SYNTAX_ERROR;
	s++;
	while(isspace(*s))
		s++;
	return *s == 0 ? 0 : SYNTAX_ERROR;
}

static int parse_materialize_args(struct tupfile *tf, const char *text, char **spec, char **root,
				  int *root_from_caller_root)
{
	const char *s = text;
	char *arg;
	int len;

	*spec = NULL;
	*root = NULL;
	if(root_from_caller_root)
		*root_from_caller_root = 0;

	while(isspace((unsigned char)*s))
		s++;
	if(strncmp(s, "materialize", 11) != 0 || !isspace((unsigned char)s[11]))
		return SYNTAX_ERROR;
	s += 11;

	while(isspace((unsigned char)*s))
		s++;
	if(*s == 0) {
		free(*spec);
		*spec = NULL;
		return SYNTAX_ERROR;
	}
	arg = malloc(strlen(s) + 1);
	if(!arg) {
		perror("malloc");
		return -1;
	}
	len = next_path(tf, s, arg);
	if(len < 0) {
		free(arg);
		return -1;
	}
	*spec = arg;
	s += len;

	while(isspace((unsigned char)*s))
		s++;
	if(*s == 0) {
		free(*spec);
		*spec = NULL;
		return SYNTAX_ERROR;
	}
	arg = malloc(strlen(s) + 1);
	if(!arg) {
		perror("malloc");
		free(*spec);
		*spec = NULL;
		return -1;
	}
	len = next_path(tf, s, arg);
	if(len < 0) {
		free(arg);
		free(*spec);
		*spec = NULL;
		return -1;
	}
	*root = arg;
	if(root_from_caller_root)
		*root_from_caller_root = path_uses_caller_path_var(tf, *root);
	s += len;

	while(isspace((unsigned char)*s))
		s++;
	if(*s != 0) {
		free(*spec);
		free(*root);
		*spec = NULL;
		*root = NULL;
		return SYNTAX_ERROR;
	}
	return 0;
}

static int vardb_clone(struct vardb *dst, struct vardb *src)
{
	struct string_tree *st;

	if(vardb_init(dst) < 0)
		return -1;
	RB_FOREACH(st, string_entries, &src->root) {
		struct var_entry *ve = container_of(st, struct var_entry, var);
		if(vardb_set(dst, ve->var.s, ve->value ? ve->value : "", ve->tent) < 0)
			return -1;
	}
	return 0;
}

static void dist_manifest_free(struct dist_manifest *dm)
{
	int x;

	if(!dm)
		return;
	for(x=0; x<dm->num_entries; x++) {
		free(dm->entries[x].src);
		free(dm->entries[x].dest);
	}
	free(dm->entries);
	memset(dm, 0, sizeof *dm);
}

static int dist_manifest_add(struct dist_manifest *dm, char *src, char *dest)
{
	struct dist_entry *tmp;
	int x;

	for(x=0; x<dm->num_entries; x++) {
		if(strcmp(dm->entries[x].dest, dest) == 0)
			return -2;
	}

	tmp = realloc(dm->entries, sizeof(*dm->entries) * (dm->num_entries + 1));
	if(!tmp) {
		perror("realloc");
		return -1;
	}
	dm->entries = tmp;
	dm->entries[dm->num_entries].src = src;
	dm->entries[dm->num_entries].dest = dest;
	dm->num_entries++;
	return 0;
}

static int dist_hex_value(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static char *dist_encode_hex(const char *s)
{
	static const char hex[] = "0123456789abcdef";
	size_t len = strlen(s);
	char *out;
	size_t x;

	out = malloc(len * 2 + 1);
	if(!out) {
		perror("malloc");
		return NULL;
	}
	for(x=0; x<len; x++) {
		unsigned char c = (unsigned char)s[x];
		out[x*2] = hex[c >> 4];
		out[x*2+1] = hex[c & 0xf];
	}
	out[len*2] = 0;
	return out;
}

static char *dist_decode_hex(const char *s, int len)
{
	char *out;
	int x;

	if((len & 1) != 0)
		return NULL;
	out = malloc(len / 2 + 1);
	if(!out) {
		perror("malloc");
		return NULL;
	}
	for(x=0; x<len; x += 2) {
		int hi = dist_hex_value((unsigned char)s[x]);
		int lo = dist_hex_value((unsigned char)s[x+1]);
		if(hi < 0 || lo < 0) {
			free(out);
			return NULL;
		}
		out[x/2] = (char)((hi << 4) | lo);
	}
	out[len/2] = 0;
	return out;
}

#define DIST_SPEC_PREFIX "__metatup_dist_v1__:"

static int dist_serialize_manifest(struct dist_manifest *dm, char **out)
{
	struct estring e;
	int x;

	if(estring_init(&e) < 0)
		return -1;
	if(estring_append(&e, DIST_SPEC_PREFIX, strlen(DIST_SPEC_PREFIX)) < 0) {
		free(e.s);
		return -1;
	}
	for(x=0; x<dm->num_entries; x++) {
		char *src = dist_encode_hex(dm->entries[x].src);
		char *dest = dist_encode_hex(dm->entries[x].dest);

		if(!src || !dest) {
			free(src);
			free(dest);
			free(e.s);
			return -1;
		}
		if(estring_append(&e, src, strlen(src)) < 0 ||
		   estring_append(&e, ">", 1) < 0 ||
		   estring_append(&e, dest, strlen(dest)) < 0 ||
		   estring_append(&e, ";", 1) < 0) {
			free(src);
			free(dest);
			free(e.s);
			return -1;
		}
		free(src);
		free(dest);
	}
	*out = e.s;
	return 0;
}

static int dist_parse_manifest(struct tupfile *tf, const char *spec, struct dist_manifest *dm)
{
	const char *s;

	memset(dm, 0, sizeof *dm);
	if(strncmp(spec, DIST_SPEC_PREFIX, strlen(DIST_SPEC_PREFIX)) != 0) {
		fprintf(tf->f, "tup error: Dist value is not a valid internal dist reference.\n");
		return -1;
	}
	s = spec + strlen(DIST_SPEC_PREFIX);
	while(*s) {
		const char *sep;
		const char *end;
		char *src;
		char *dest;
		int rc;

		sep = strchr(s, '>');
		end = strchr(s, ';');
		if(!sep || !end || end < sep) {
			fprintf(tf->f, "tup error: Dist value is malformed.\n");
			goto err;
		}
		src = dist_decode_hex(s, sep - s);
		dest = dist_decode_hex(sep + 1, end - sep - 1);
		if(!src || !dest) {
			free(src);
			free(dest);
			fprintf(tf->f, "tup error: Dist value is malformed.\n");
			goto err;
		}
		rc = dist_manifest_add(dm, src, dest);
		if(rc == -2) {
			fprintf(tf->f, "tup error: Dist contains duplicate destination '%s'.\n", dest);
			free(src);
			free(dest);
			goto err;
		} else if(rc < 0) {
			free(src);
			free(dest);
			goto err;
		}
		s = end + 1;
	}
	return 0;
err:
	dist_manifest_free(dm);
	return -1;
}

static const char *find_top_level_keyword(const char *s, const char *kw)
{
	int paren = 0;
	int brace = 0;
	int bracket = 0;
	int in_quote = 0;
	int escaped = 0;
	size_t kwlen = strlen(kw);

	for(; *s; s++) {
		if(in_quote) {
			if(escaped) {
				escaped = 0;
			} else if(*s == '\\') {
				escaped = 1;
			} else if(*s == '"') {
				in_quote = 0;
			}
			continue;
		}
		if(*s == '"') {
			in_quote = 1;
		} else if(*s == '(') {
			paren++;
		} else if(*s == ')' && paren > 0) {
			paren--;
		} else if(*s == '{') {
			brace++;
		} else if(*s == '}' && brace > 0) {
			brace--;
		} else if(*s == '[') {
			bracket++;
		} else if(*s == ']' && bracket > 0) {
			bracket--;
		} else if(paren == 0 && brace == 0 && bracket == 0 &&
			  strncmp(s, kw, kwlen) == 0) {
			return s;
		}
	}
	return NULL;
}

static const char *find_next_dist_stmt(const char *s)
{
	int paren = 0;
	int brace = 0;
	int bracket = 0;
	int in_quote = 0;
	int escaped = 0;

	for(; *s; s++) {
		if(in_quote) {
			if(escaped) {
				escaped = 0;
			} else if(*s == '\\') {
				escaped = 1;
			} else if(*s == '"') {
				in_quote = 0;
			}
			continue;
		}
		if(*s == '"') {
			in_quote = 1;
		} else if(*s == '(') {
			paren++;
		} else if(*s == ')' && paren > 0) {
			paren--;
		} else if(*s == '{') {
			brace++;
		} else if(*s == '}' && brace > 0) {
			brace--;
		} else if(*s == '[') {
			bracket++;
		} else if(*s == ']' && bracket > 0) {
			bracket--;
		} else if(paren == 0 && brace == 0 && bracket == 0 &&
			  isspace((unsigned char)*s)) {
			const char *next = s;
			int count = 0;

			while(isspace((unsigned char)*next)) {
				count++;
				next++;
			}
			if(count >= 2 &&
			   (strncmp(next, "at ", 3) == 0 ||
			    strncmp(next, "mounts ", 7) == 0))
				return next;
		}
	}
	return NULL;
}

static char *dup_trimmed_range(const char *start, const char *end)
{
	while(start < end && isspace((unsigned char)*start))
		start++;
	while(end > start && isspace((unsigned char)end[-1]))
		end--;
	return strndup(start, end - start);
}

static char *dist_eval_expr(struct tupfile *tf, const char *expr, int expand_nodes)
{
	char *raw;
	char *out;
	size_t len = strlen(expr);

	if(len >= 2 && expr[0] == '"' && expr[len-1] == '"') {
		raw = strndup(expr + 1, len - 2);
		if(!raw) {
			perror("strndup");
			return NULL;
		}
		out = eval(tf, raw, expand_nodes);
		free(raw);
		return out;
	}
	return eval(tf, expr, expand_nodes);
}

static int function_arg_set_reldir(struct tupfile *tf, struct vardb *args)
{
	char path[PATH_MAX];
	const char *reldir;

	if(tf->curtent && tf->curtent->tnode.tupid != DOT_DT) {
		if(snprint_tup_entry(path, sizeof(path), tf->curtent) >= (signed)sizeof(path)) {
			fprintf(tf->f, "tup error: Current function directory is too long.\n");
			return -1;
		}
		if(path[0] == '/')
			reldir = path + 1;
		else
			reldir = path;
	} else {
		reldir = ".";
	}
	if(vardb_set(args, "reldir", reldir, NULL) < 0)
		return -1;
	return 0;
}

static int function_arg_set_brdir(struct vardb *args)
{
	struct var_entry *builddir;
	struct var_entry *reldir;
	char *brdir;

	builddir = vardb_get(args, "builddir", strlen("builddir"));
	if(!builddir || !builddir->value)
		return 0;
	reldir = vardb_get(args, "reldir", strlen("reldir"));
	if(!reldir || !reldir->value)
		return 0;
	if(tupbuild_combine_builddir_reldir(builddir->value, reldir->value, &brdir) < 0)
		return -1;
	if(vardb_set(args, "brdir", brdir, NULL) < 0) {
		free(brdir);
		return -1;
	}
	free(brdir);
	return 0;
}

static int canonicalize_path_simple(const char *path, char **out)
{
	int absolute = is_full_path(path);
	char *copy;
	char *saveptr = NULL;
	char *tok;
	char **parts = NULL;
	int count = 0;
	int cap = 0;
	int i;
	int len = absolute ? 1 : 0;

	copy = strdup(path);
	if(!copy) {
		perror("strdup");
		return -1;
	}

	for(tok = strtok_r(copy, "/", &saveptr); tok; tok = strtok_r(NULL, "/", &saveptr)) {
		if(strcmp(tok, ".") == 0 || tok[0] == 0)
			continue;
		if(strcmp(tok, "..") == 0) {
			if(count > 0 && strcmp(parts[count-1], "..") != 0) {
				count--;
				continue;
			}
			if(absolute)
				continue;
		}
		if(count == cap) {
			int newcap = cap ? cap * 2 : 8;
			char **tmp = realloc(parts, sizeof(*parts) * newcap);
			if(!tmp) {
				perror("realloc");
				free(parts);
				free(copy);
				return -1;
			}
			parts = tmp;
			cap = newcap;
		}
		parts[count++] = tok;
		len += strlen(tok) + ((absolute || count > 1) ? 1 : 0);
	}

	if(count == 0 && !absolute) {
		*out = strdup(".");
		free(parts);
		free(copy);
		if(!*out) {
			perror("strdup");
			return -1;
		}
		return 0;
	}

	*out = malloc(len + 1);
	if(!*out) {
		perror("malloc");
		free(parts);
		free(copy);
		return -1;
	}
	len = 0;
	if(absolute)
		(*out)[len++] = '/';
	for(i=0; i<count; i++) {
		int plen = strlen(parts[i]);
		if(i > 0)
			(*out)[len++] = '/';
		memcpy(*out + len, parts[i], plen);
		len += plen;
	}
	if(len == 0 && absolute)
		(*out)[len++] = '/';
	(*out)[len] = 0;

	free(parts);
	free(copy);
	return 0;
}

static int dist_normalize_dest(struct tupfile *tf, const char *base, const char *name, char **out)
{
	char *joined;
	char *canon;
	size_t len = strlen(base) + strlen(name) + 2;

	joined = malloc(len);
	if(!joined) {
		perror("malloc");
		return -1;
	}
	snprintf(joined, len, "%s/%s", base, name);
	if(canonicalize_path_simple(joined, &canon) < 0) {
		free(joined);
		return -1;
	}
	free(joined);
	if(canon[0] != '/') {
		fprintf(tf->f, "tup error: Dist destinations must resolve to absolute paths, but got '%s'.\n", canon);
		free(canon);
		return -1;
	}
	*out = canon;
	return 0;
}

static int dist_normalize_dest_path(struct tupfile *tf, const char *path, char **out)
{
	char *canon;

	if(canonicalize_path_simple(path, &canon) < 0)
		return -1;
	if(canon[0] != '/') {
		fprintf(tf->f, "tup error: Dist destinations must resolve to absolute paths, but got '%s'.\n", canon);
		free(canon);
		return -1;
	}
	*out = canon;
	return 0;
}

static int dist_parse_mapping_stmt(struct tupfile *tf, const char *stmt, struct dist_manifest *dm)
{
	const char *src_end;
	const char *srcbase_end;
	const char *dstbase_end;
	char *srcbase_raw = NULL;
	char *src_raw = NULL;
	char *dstbase_raw = NULL;
	char *dstname_raw = NULL;
	char *dstpath_raw = NULL;
	char *srcbase = NULL;
	char *src = NULL;
	char *dstbase = NULL;
	char *dstname = NULL;
	char *dstpath = NULL;
	char *srcdup = NULL;
	char *dest = NULL;
	int rc;

	if(strncmp(stmt, "at ", 3) != 0) {
		fprintf(tf->f, "tup error: Dist entries must start with 'at'.\n");
		return -1;
	}
	stmt += 3;
	src_end = find_top_level_keyword(stmt, " => ");
	if(!src_end) {
		fprintf(tf->f, "tup error: Dist entry is missing '=>'.\n");
		return -1;
	}
	srcbase_end = find_top_level_keyword(stmt, " as ");
	if(srcbase_end && srcbase_end < src_end) {
		srcbase_raw = dup_trimmed_range(stmt, srcbase_end);
		if(!srcbase_raw) {
			perror("strndup");
			return -1;
		}
		src_raw = dup_trimmed_range(srcbase_end + 4, src_end);
	} else {
		src_raw = dup_trimmed_range(stmt, src_end);
	}
	if(!src_raw) {
		perror("strndup");
		goto err;
	}
	stmt = src_end + 4;
	if(strncmp(stmt, "at ", 3) != 0) {
		fprintf(tf->f, "tup error: Dist destination must start with 'at'.\n");
		goto err;
	}
	stmt += 3;
	dstbase_end = find_top_level_keyword(stmt, " as ");
	if(dstbase_end) {
		dstbase_raw = dup_trimmed_range(stmt, dstbase_end);
		dstname_raw = dup_trimmed_range(dstbase_end + 4, stmt + strlen(stmt));
		if(!dstbase_raw || !dstname_raw) {
			perror("strndup");
			goto err;
		}
	} else {
		dstpath_raw = dup_trimmed_range(stmt, stmt + strlen(stmt));
		if(!dstpath_raw) {
			perror("strndup");
			goto err;
		}
	}
	if(srcbase_raw) {
		srcbase = dist_eval_expr(tf, srcbase_raw, EXPAND_NODES);
		if(!srcbase)
			goto err;
	}
	src = dist_eval_expr(tf, src_raw, EXPAND_NODES);
	if(dstbase_raw) {
		dstbase = dist_eval_expr(tf, dstbase_raw, KEEP_NODES);
		dstname = dist_eval_expr(tf, dstname_raw, KEEP_NODES);
		if(!dstbase || !dstname)
			goto err;
	} else {
		dstpath = dist_eval_expr(tf, dstpath_raw, KEEP_NODES);
		if(!dstpath)
			goto err;
	}
	if(!src)
		goto err;
	if(src[0] == 0 ||
	   (srcbase && srcbase[0] == 0) ||
	   (dstbase && dstbase[0] == 0) ||
	   (dstname && dstname[0] == 0) ||
	   (dstpath && dstpath[0] == 0)) {
		fprintf(tf->f, "tup error: Dist mapping expressions must not be empty.\n");
		goto err;
	}
	if(dstpath) {
		if(dist_normalize_dest_path(tf, dstpath, &dest) < 0)
			goto err;
	} else {
		if(dist_normalize_dest(tf, dstbase, dstname, &dest) < 0)
			goto err;
	}
	srcdup = strdup(src);
	if(!srcdup) {
		perror("strdup");
		goto err;
	}
	rc = dist_manifest_add(dm, srcdup, dest);
	if(rc == -2) {
		fprintf(tf->f, "tup error: Dist contains duplicate destination '%s'.\n", dest);
		free(srcdup);
		free(dest);
		dest = NULL;
		goto err;
	} else if(rc < 0) {
		free(srcdup);
		free(dest);
		dest = NULL;
		goto err;
	}

	free(srcbase_raw);
	free(src_raw);
	free(dstbase_raw);
	free(dstname_raw);
	free(dstpath_raw);
	free(srcbase);
	free(src);
	free(dstbase);
	free(dstname);
	free(dstpath);
	return 0;
err:
	free(srcbase_raw);
	free(src_raw);
	free(dstbase_raw);
	free(dstname_raw);
	free(dstpath_raw);
	free(srcbase);
	free(src);
	free(dstbase);
	free(dstname);
	free(dstpath);
	free(dest);
	return -1;
}

static int dist_parse_mount_stmt(struct tupfile *tf, const char *stmt, struct dist_manifest *dm)
{
	const char *at_kw;
	char *spec_raw = NULL;
	char *prefix_raw = NULL;
	char *spec = NULL;
	char *prefix = NULL;
	struct dist_manifest mounted;
	int x;

	memset(&mounted, 0, sizeof mounted);
	if(strncmp(stmt, "mounts ", 7) != 0) {
		fprintf(tf->f, "tup error: Dist mount entries must start with 'mounts'.\n");
		return -1;
	}
	stmt += 7;
	at_kw = find_top_level_keyword(stmt, " at ");
	if(!at_kw) {
		fprintf(tf->f, "tup error: Dist mount entry is missing 'at'.\n");
		return -1;
	}
	spec_raw = dup_trimmed_range(stmt, at_kw);
	prefix_raw = dup_trimmed_range(at_kw + 4, stmt + strlen(stmt));
	if(!spec_raw || !prefix_raw) {
		perror("strndup");
		goto err;
	}
	spec = dist_eval_expr(tf, spec_raw, KEEP_NODES);
	prefix = dist_eval_expr(tf, prefix_raw, KEEP_NODES);
	if(!spec || !prefix)
		goto err;
	if(dist_parse_manifest(tf, spec, &mounted) < 0)
		goto err;
	for(x=0; x<mounted.num_entries; x++) {
		char *dest;
		char *srcdup;
		int rc;

		if(dist_normalize_dest(tf, prefix, mounted.entries[x].dest, &dest) < 0)
			goto err;
		srcdup = strdup(mounted.entries[x].src);
		if(!srcdup) {
			perror("strdup");
			free(dest);
			goto err;
		}
		rc = dist_manifest_add(dm, srcdup, dest);
		if(rc == -2) {
			fprintf(tf->f, "tup error: Dist contains duplicate destination '%s'.\n", dest);
			free(srcdup);
			free(dest);
			goto err;
		} else if(rc < 0) {
			free(srcdup);
			free(dest);
			goto err;
		}
	}

	free(spec_raw);
	free(prefix_raw);
	free(spec);
	free(prefix);
	dist_manifest_free(&mounted);
	return 0;
err:
	free(spec_raw);
	free(prefix_raw);
	free(spec);
	free(prefix);
	dist_manifest_free(&mounted);
	return -1;
}

static int parse_dist_block(struct tupfile *tf, const char *text, char **out)
{
	const char *brace;
	const char *end;
	const char *keyword = NULL;
	int keyword_len = 0;
	char *body;
	const char *s;
	struct dist_manifest dm;

	memset(&dm, 0, sizeof dm);
	while(isspace((unsigned char)*text))
		text++;
	if(strncmp(text, "dist", 4) == 0) {
		keyword = "dist";
		keyword_len = 4;
	} else if(strncmp(text, "dest", 4) == 0) {
		keyword = "dest";
		keyword_len = 4;
	} else {
		fprintf(tf->f, "tup error: Dist binding must start with 'dist' or 'dest'.\n");
		return -1;
	}
	text += keyword_len;
	while(isspace((unsigned char)*text))
		text++;
	if(*text != '{') {
		fprintf(tf->f, "tup error: %s binding requires a '{ ... }' block.\n", keyword);
		return -1;
	}
	brace = text;
	end = brace + strlen(brace);
	while(end > brace && isspace((unsigned char)end[-1]))
		end--;
	if(end <= brace || end[-1] != '}') {
		fprintf(tf->f, "tup error: %s binding requires a closing '}'.\n", keyword);
		return -1;
	}
	body = dup_trimmed_range(brace + 1, end - 1);
	if(!body) {
		perror("strndup");
		return -1;
	}
	s = body;
	while(*s) {
		const char *next;
		char *stmt;
		int rc;

		while(isspace((unsigned char)*s))
			s++;
		if(*s == 0)
			break;
		next = find_next_dist_stmt(s + 1);
		if(next) {
			stmt = dup_trimmed_range(s, next);
			s = next;
		} else {
			stmt = strdup(s);
			s += strlen(s);
		}
		if(!stmt) {
			perror("strdup");
			free(body);
			dist_manifest_free(&dm);
			return -1;
		}
		if(strncmp(stmt, "at ", 3) == 0) {
			rc = dist_parse_mapping_stmt(tf, stmt, &dm);
		} else if(strncmp(stmt, "mounts ", 7) == 0) {
			rc = dist_parse_mount_stmt(tf, stmt, &dm);
		} else {
			fprintf(tf->f, "tup error: Unknown %s statement '%s'.\n", keyword, stmt);
			rc = -1;
		}
		free(stmt);
		if(rc < 0) {
			free(body);
			dist_manifest_free(&dm);
			return -1;
		}
	}
	free(body);
	if(dist_serialize_manifest(&dm, out) < 0) {
		dist_manifest_free(&dm);
		return -1;
	}
	dist_manifest_free(&dm);
	return 0;
}

static const char *skip_glob_slashes(const char *s)
{
	while(*s == '/')
		s++;
	return s;
}

static const char *next_glob_segment(const char *s, const char **seg, int *len)
{
	s = skip_glob_slashes(s);
	*seg = s;
	while(*s && *s != '/')
		s++;
	*len = s - *seg;
	return s;
}

static int glob_segment_is_doublestar(const char *seg, int len)
{
	return len == 2 && seg[0] == '*' && seg[1] == '*';
}

static int glob_path_match_components(const char *path, const char *pattern)
{
	const char *pseg;
	const char *sseg;
	const char *pnext;
	const char *snext;
	int plen;
	int slen;
	char pbuf[PATH_MAX];
	char sbuf[PATH_MAX];

	pnext = next_glob_segment(pattern, &pseg, &plen);
	snext = next_glob_segment(path, &sseg, &slen);
	if(plen == 0)
		return slen == 0;
	if(glob_segment_is_doublestar(pseg, plen)) {
		if(glob_path_match_components(path, pnext))
			return 1;
		while(slen != 0) {
			path = snext;
			snext = next_glob_segment(path, &sseg, &slen);
			if(glob_path_match_components(path, pnext))
				return 1;
		}
		return 0;
	}
	if(slen == 0)
		return 0;
	if(plen >= (int)sizeof(pbuf) || slen >= (int)sizeof(sbuf))
		return 0;
	memcpy(pbuf, pseg, plen);
	pbuf[plen] = 0;
	memcpy(sbuf, sseg, slen);
	sbuf[slen] = 0;
	if(fnmatch(pbuf, sbuf, FNM_PERIOD) != 0)
		return 0;
	return glob_path_match_components(snext, pnext);
}

static int glob_path_match(const char *path, const char *pattern)
{
	if(is_full_path(pattern) != is_full_path(path))
		return 0;
	return glob_path_match_components(path, pattern);
}

static int glob_pattern_has_magic(const char *pattern)
{
	for(; *pattern; pattern++) {
		if(*pattern == '*' || *pattern == '?' || *pattern == '[')
			return 1;
	}
	return 0;
}

static int glob_root_from_pattern(const char *pattern, char **out)
{
	const char *seg;
	const char *next;
	const char *root_end = pattern;
	int len;
	int absolute = is_full_path(pattern);

	next = pattern;
	if(absolute) {
		while(*next == '/')
			next++;
		root_end = pattern + 1;
	}
	while(1) {
		next = next_glob_segment(next, &seg, &len);
		if(len == 0)
			break;
		if(glob_segment_is_doublestar(seg, len) || char_find(seg, len, "*?[")) {
			break;
		}
		root_end = next;
	}
	if(root_end == pattern) {
		if(absolute)
			*out = strdup("/");
		else
			*out = strdup(".");
	} else {
		*out = strndup(pattern, root_end - pattern);
	}
	if(!*out) {
		perror("strdup");
		return -1;
	}
	return 0;
}

static int eval_abs_one_path(struct tupfile *tf, const char *path, int path_from_caller_root,
			     struct estring *e, int *first)
{
	char cwd[PATH_MAX];
	char base[PATH_MAX];
	char *joined = NULL;
	char *canon = NULL;
	struct tup_build_ctx *ctx = tf->func_frame ? tf->func_frame->build_ctx : NULL;

	if(is_full_path(path)) {
		joined = strdup(path);
		if(!joined) {
			perror("strdup");
			return -1;
		}
		} else {
			if(path_from_caller_root) {
				base[0] = 0;
				if(ctx && ctx->caller_base_tent) {
					if(snprint_tup_entry(base, sizeof(base), ctx->caller_base_tent) >= (signed)sizeof(base)) {
						fprintf(tf->f, "tup error: Caller base directory is too long.\n");
						return -1;
					}
				}
				joined = malloc(strlen(get_tup_top()) + strlen(base) + strlen(path) + 3);
				if(!joined) {
					perror("malloc");
					return -1;
				}
				if(base[0] == 0 || strcmp(base, ".") == 0)
					snprintf(joined, strlen(get_tup_top()) + strlen(base) + strlen(path) + 3, "%s/%s", get_tup_top(), path);
				else
					snprintf(joined, strlen(get_tup_top()) + strlen(base) + strlen(path) + 3, "%s/%s/%s", get_tup_top(), base, path);
			} else {
			if(snprint_tup_entry(cwd, sizeof(cwd), tf->curtent) >= (signed)sizeof(cwd)) {
				fprintf(tf->f, "tup error: Current function directory is too long.\n");
				return -1;
			}
			joined = malloc(strlen(get_tup_top()) + strlen(cwd) + strlen(path) + 3);
			if(!joined) {
				perror("malloc");
				return -1;
			}
			if(strcmp(cwd, ".") == 0 || cwd[0] == 0)
				snprintf(joined, strlen(get_tup_top()) + strlen(cwd) + strlen(path) + 3, "%s/%s", get_tup_top(), path);
			else
				snprintf(joined, strlen(get_tup_top()) + strlen(cwd) + strlen(path) + 3, "%s/%s/%s", get_tup_top(), cwd, path);
		}
	}
	if(canonicalize_path_simple(joined, &canon) < 0) {
		free(joined);
		return -1;
	}
	if(!*first) {
		if(estring_append(e, " ", 1) < 0) {
			free(joined);
			free(canon);
			return -1;
		}
	}
	if(estring_append(e, canon, strlen(canon)) < 0) {
		free(joined);
		free(canon);
		return -1;
	}
	*first = 0;
	free(joined);
	free(canon);
	return 0;
}

static int eval_abs_function(struct tupfile *tf, const char *args, int argslen, struct estring *e)
{
	char *copy;
	char *expanded = NULL;
	const char *s;
	int first = 1;

	copy = strndup(args, argslen);
	if(!copy) {
		perror("strndup");
		return -1;
	}
	s = copy;
	while(*s) {
		char path[PATH_MAX];
		int path_from_caller_root;
		int rc;

		while(isspace(*s))
			s++;
		if(*s == 0)
			break;
		rc = next_path(tf, s, path);
		if(rc < 0) {
			free(copy);
			return -1;
		}
		path_from_caller_root = path_uses_caller_path_var(tf, path);
		expanded = eval(tf, path, KEEP_NODES);
		if(!expanded) {
			free(copy);
			return -1;
		}
		if(eval_abs_one_path(tf, expanded, path_from_caller_root, e, &first) < 0) {
			free(expanded);
			free(copy);
			return -1;
		}
		free(expanded);
		s += rc;
	}
	free(copy);
	return 0;
}

static int eval_realname_function(struct tupfile *tf, const char *args, int argslen, struct estring *e)
{
	char *copy;
	char *expanded;
	const char *base;

	copy = strndup(args, argslen);
	if(!copy) {
		perror("strndup");
		return -1;
	}
	expanded = eval(tf, copy, KEEP_NODES);
	free(copy);
	if(!expanded)
		return -1;
	base = strrchr(expanded, '/');
	if(base)
		base++;
	else
		base = expanded;
	if(estring_append(e, base, strlen(base)) < 0) {
		free(expanded);
		return -1;
	}
	free(expanded);
	return 0;
}

static int eval_globs_append_match(const char *path, struct estring *e, int *first)
{
	if(!*first) {
		if(estring_append(e, " ", 1) < 0)
			return -1;
	}
	if(estring_append(e, path, strlen(path)) < 0)
		return -1;
	*first = 0;
	return 0;
}

static int eval_globs_walk(const char *root, const char *pattern, struct estring *e, int *first)
{
	struct flist f = FLIST_INITIALIZER;
	struct stat st;

	if(lstat(root, &st) < 0)
		return 0;
	if(glob_path_match(root, pattern)) {
		if(eval_globs_append_match(root, e, first) < 0)
			return -1;
	}
	if(!S_ISDIR(st.st_mode))
		return 0;

	flist_foreach(&f, root) {
		char *child;
		int len;

		if(strcmp(f.filename, ".") == 0 || strcmp(f.filename, "..") == 0)
			continue;
		len = strlen(root) + 1 + strlen(f.filename);
		child = malloc(len + 1);
		if(!child) {
			perror("malloc");
			return -1;
		}
		snprintf(child, len + 1, "%s/%s", root, f.filename);
		if(eval_globs_walk(child, pattern, e, first) < 0) {
			free(child);
			return -1;
		}
		free(child);
	}
	return 0;
}

static int eval_globs_one_pattern(struct tupfile *tf, const char *pattern, struct estring *e, int *first)
{
	char cwd[PATH_MAX];
	char *joined = NULL;
	char *canon = NULL;
	char *root = NULL;
	struct stat st;

	if(is_full_path(pattern)) {
		joined = strdup(pattern);
		if(!joined) {
			perror("strdup");
			return -1;
		}
	} else {
		if(snprint_tup_entry(cwd, sizeof(cwd), tf->curtent) >= (signed)sizeof(cwd)) {
			fprintf(tf->f, "tup error: Current function directory is too long.\n");
			return -1;
		}
		joined = malloc(strlen(get_tup_top()) + strlen(cwd) + strlen(pattern) + 3);
		if(!joined) {
			perror("malloc");
			return -1;
		}
		if(strcmp(cwd, ".") == 0 || cwd[0] == 0)
			snprintf(joined, strlen(get_tup_top()) + strlen(cwd) + strlen(pattern) + 3, "%s/%s", get_tup_top(), pattern);
		else
			snprintf(joined, strlen(get_tup_top()) + strlen(cwd) + strlen(pattern) + 3, "%s/%s/%s", get_tup_top(), cwd, pattern);
	}
	if(canonicalize_path_simple(joined, &canon) < 0)
		goto err;
	if(!glob_pattern_has_magic(canon)) {
		if(lstat(canon, &st) == 0) {
			if(eval_globs_append_match(canon, e, first) < 0)
				goto err;
		}
		free(joined);
		free(canon);
		return 0;
	}
	if(glob_root_from_pattern(canon, &root) < 0)
		goto err;
	if(eval_globs_walk(root, canon, e, first) < 0)
		goto err;
	free(joined);
	free(canon);
	free(root);
	return 0;
err:
	free(joined);
	free(canon);
	free(root);
	return -1;
}

static int eval_globs_function(struct tupfile *tf, const char *args, int argslen, struct estring *e)
{
	char *copy;
	char *expanded = NULL;
	const char *s;
	int first = 1;

	copy = strndup(args, argslen);
	if(!copy) {
		perror("strndup");
		return -1;
	}
	s = copy;
	while(*s) {
		char path[PATH_MAX];
		int rc;

		while(isspace(*s))
			s++;
		if(*s == 0)
			break;
		rc = next_path(tf, s, path);
		if(rc < 0) {
			free(copy);
			return -1;
		}
		expanded = eval(tf, path, KEEP_NODES);
		if(!expanded) {
			free(copy);
			return -1;
		}
		if(eval_globs_one_pattern(tf, expanded, e, &first) < 0) {
			free(expanded);
			free(copy);
			return -1;
		}
		free(expanded);
		s += rc;
	}
	free(copy);
	return 0;
}

static int eval_groups_append_path(struct tup_entry *tent, const char *path, int pathlen,
				   struct estring *e, int *first)
{
	if(!*first) {
		if(estring_append(e, " ", 1) < 0)
			return -1;
	}
	if(estring_append(e, get_tup_top(), get_tup_top_len()) < 0)
		return -1;
	if(tent) {
		char fullpath[PATH_MAX];
		int rc;

		rc = snprint_tup_entry(fullpath, sizeof(fullpath), tent);
		if(rc >= (int)sizeof(fullpath)) {
			fprintf(stderr, "tup error: Bin output path is too long.\n");
			return -1;
		}
		if(estring_append(e, fullpath, rc) < 0)
			return -1;
	} else {
		if(!is_full_path(path)) {
			if(estring_append(e, "/", 1) < 0)
				return -1;
		}
		if(estring_append(e, path, pathlen) < 0)
			return -1;
	}
	*first = 0;
	return 0;
}

static int eval_groups_one(struct tupfile *tf, const char *group, int grouplen,
			   struct estring *e, int *first)
{
	struct bin *b;
	struct bin_entry *be;
	char binname[PATH_MAX];

	if(grouplen <= 0 || grouplen >= (int)sizeof(binname)) {
		fprintf(tf->f, "tup error: Invalid bin name in $(groups ...).\n");
		return -1;
	}
	memcpy(binname, group, grouplen);
	binname[grouplen] = 0;

	b = bin_find(binname, current_bin_head(tf));
	if(!b) {
		fprintf(tf->f, "tup error: Unable to find bin '{%s}' in $(groups ...).\n", binname);
		return -1;
	}
	TAILQ_FOREACH(be, &b->entries, list) {
		if(eval_groups_append_path(be->tent, be->path, be->len, e, first) < 0)
			return -1;
	}
	return 0;
}

static int eval_groups_function(struct tupfile *tf, const char *args, int argslen, struct estring *e)
{
	const char *s = args;
	const char *end = args + argslen;
	int first = 1;

	while(s < end) {
		const char *group;

		while(s < end && isspace(*s))
			s++;
		if(s == end)
			break;
		if(*s != '{') {
			fprintf(tf->f, "tup error: $(groups ...) arguments must use {group} syntax.\n");
			return -1;
		}
		group = ++s;
		while(s < end && *s != '}')
			s++;
		if(s == end) {
			fprintf(tf->f, "tup error: Missing closing '}' in $(groups ...).\n");
			return -1;
		}
		if(eval_groups_one(tf, group, s - group, e, &first) < 0)
			return -1;
		s++;
		while(s < end && isspace(*s))
			s++;
		if(s < end && *s != '{') {
			fprintf(tf->f, "tup error: $(groups ...) arguments must be space-separated {group} values.\n");
			return -1;
		}
	}
	return 0;
}

static int move_name_list(struct name_list *dst, struct name_list *src)
{
	while(!TAILQ_EMPTY(&src->entries)) {
		struct name_list_entry *nle;

		nle = TAILQ_FIRST(&src->entries);
		move_name_list_entry(dst, src, nle);
	}
	return 0;
}

static int tent_is_under_dir(struct tup_entry *tent, struct tup_entry *root)
{
	while(tent) {
		if(tent == root)
			return 1;
		tent = tent->parent;
	}
	return 0;
}

static int build_ctx_init(struct tup_build_ctx *ctx)
{
	TAILQ_INIT(&ctx->order_only_input_paths);
	init_name_list(&ctx->outputs);
	RB_INIT(&ctx->caller_path_vars);
	ctx->strict = 0;
	ctx->caller_base_tent = NULL;
	return 0;
}

static void build_ctx_close(struct tup_build_ctx *ctx)
{
	free_path_list(&ctx->order_only_input_paths);
	delete_name_list(&ctx->outputs);
	free_string_tree(&ctx->caller_path_vars);
}

static int build_ctx_add_caller_path_var(struct tup_build_ctx *ctx, const char *name)
{
	struct string_tree *st;

	st = string_tree_search(&ctx->caller_path_vars, name, strlen(name));
	if(st)
		return 0;
	st = malloc(sizeof *st);
	if(!st) {
		perror("malloc");
		return -1;
	}
	if(string_tree_add(&ctx->caller_path_vars, st, name) < 0) {
		free(st);
		return -1;
	}
	return 0;
}

static int path_uses_caller_path_var(struct tupfile *tf, const char *path)
{
	struct tup_build_ctx *ctx = tf->func_frame ? tf->func_frame->build_ctx : NULL;
	struct string_tree *st;

	if(!ctx)
		return 0;
	RB_FOREACH(st, string_entries, &ctx->caller_path_vars) {
		char pattern[PATH_MAX];
		int rc;

		rc = snprintf(pattern, sizeof(pattern), "$(%s)", st->s);
		if(rc < 0 || rc >= (int)sizeof(pattern))
			continue;
		if(strstr(path, pattern) != NULL)
			return 1;
	}
	return 0;
}

static int build_ctx_strict_check(struct tupfile *tf, struct path_list *pl)
{
	struct tup_build_ctx *ctx = tf->func_frame ? tf->func_frame->build_ctx : NULL;
	struct var_entry *ve;
	struct tup_entry *dtent;
	struct tup_entry *strict_tent;

	if(!ctx || !ctx->strict)
		return 0;
	ve = tf->func_frame ? vardb_get(&tf->func_frame->args, "brdir", strlen("brdir")) : NULL;
	if(!ve || !ve->value)
		return 0;
	if(tupbuild_resolve_builddir(tf, ve->value, &strict_tent) < 0)
		return -1;
	if(tup_entry_add(pl->dt, &dtent) < 0)
		return -1;
	if(tent_is_under_dir(dtent, strict_tent))
		return 0;

	fprintf(tf->f, "tup error: YAML strict mode forbids output '%s' outside builddir '", pl->mem);
	print_tup_entry(tf->f, strict_tent);
	fprintf(tf->f, "'.\n");
	return -1;
}

static int build_ctx_copy_order_only_inputs(struct tupfile *tf, struct rule *r)
{
	struct tup_build_ctx *ctx = tf->func_frame ? tf->func_frame->build_ctx : NULL;

	if(!ctx)
		return 0;
	return copy_path_list(tf, &r->order_only_input_paths, &ctx->order_only_input_paths);
}

static int build_ctx_add_outputs_as_order_only_inputs(struct tupfile *tf,
						       struct tup_build_ctx *ctx,
						       struct name_list_entry *old_last_output)
{
	struct name_list_entry *nle;
	struct path_list *pl;
	int orderid = 1;

	if(!ctx)
		return 0;

	TAILQ_FOREACH(pl, &ctx->order_only_input_paths, list) {
		if(pl->orderid >= orderid)
			orderid = pl->orderid + 1;
	}

	if(old_last_output)
		nle = TAILQ_NEXT(old_last_output, list);
	else
		nle = TAILQ_FIRST(&ctx->outputs.entries);

	while(nle) {
		pl = new_pl(tf, nle->path, -1, NULL, orderid++);
		if(!pl)
			return -1;
		pl->tent_relative = 0;
		TAILQ_INSERT_TAIL(&ctx->order_only_input_paths, pl, list);
		nle = TAILQ_NEXT(nle, list);
	}

	return 0;
}

static int parse_rules_block(struct tupfile *tf, const char *body, int len, const char *filename)
{
	struct buf b;
	int old_rules = tf->in_rules_block;
	int rc;

	b.s = malloc(len + 1);
	if(!b.s) {
		perror("malloc");
		return -1;
	}
	memcpy(b.s, body, len);
	b.s[len] = 0;
	b.len = len;
	tf->in_rules_block = 1;
	rc = parse_tupfile(tf, &b, filename);
	tf->in_rules_block = old_rules;
	free(b.s);
	return rc;
}

static int parse_quoted_string(const char **sp, char **out)
{
	const char *s = *sp;
	const char *start;
	int len;

	if(*s != '"')
		return SYNTAX_ERROR;
	s++;
	start = s;
	while(*s && *s != '"') {
		if(*s == '\\' && s[1])
			s += 2;
		else
			s++;
	}
	if(*s != '"')
		return SYNTAX_ERROR;
	len = s - start;
	*out = malloc(len + 1);
	if(!*out) {
		perror("malloc");
		return -1;
	}
	memcpy(*out, start, len);
	(*out)[len] = 0;
	*sp = s + 1;
	return 0;
}

static const char *find_matching_paren(const char *s)
{
	int depth = 1;

	for(; *s; s++) {
		if(*s == '\\') {
			if(s[1] != 0)
				s++;
			continue;
		}
		if(*s == '(') {
			depth++;
		} else if(*s == ')') {
			depth--;
			if(depth == 0)
				return s;
		}
	}
	return NULL;
}

static int merge_arg_frame(struct vardb *dst, struct vardb *src)
{
	struct string_tree *st;

	if(!src)
		return 0;
	RB_FOREACH(st, string_entries, &src->root) {
		struct var_entry *ve = container_of(st, struct var_entry, var);
		if(vardb_set(dst, ve->var.s, ve->value ? ve->value : "", NULL) < 0)
			return -1;
	}
	return 0;
}

static int parse_arg_map(struct tupfile *tf, const char *text, struct vardb *args, int allow_spread)
{
	const char *s = text;
	int first_entry = 1;

	if(vardb_init(args) < 0)
		return -1;
	while(isspace(*s))
		s++;
	if(*s != '{')
		return SYNTAX_ERROR;
	s++;
	for(;;) {
		char *key = NULL;
		char *val = NULL;
		char *evalval;

		while(isspace(*s) || *s == ',')
			s++;
		if(*s == '}')
			break;
		if(allow_spread && first_entry && tf->func_frame && strncmp(s, "...", 3) != 0) {
			fprintf(tf->f, "tup error: Nested function calls must begin their argument map with '...'.\n");
			return -1;
		}
		if(allow_spread && strncmp(s, "...", 3) == 0) {
			if(tf->func_frame && merge_arg_frame(args, &tf->func_frame->args) < 0)
				return -1;
			s += 3;
			first_entry = 0;
			continue;
		}
		if(parse_quoted_string(&s, &key) < 0)
			return SYNTAX_ERROR;
		while(isspace(*s))
			s++;
		if(*s != ':') {
			free(key);
			return SYNTAX_ERROR;
		}
		s++;
		while(isspace(*s))
			s++;
		if(*s == '"') {
			if(parse_quoted_string(&s, &val) < 0) {
				free(key);
				return SYNTAX_ERROR;
			}
		} else {
			const char *start = s;
			while(*s && *s != ',' && *s != '}')
				s++;
			val = malloc((s-start) + 1);
			if(!val) {
				perror("malloc");
				free(key);
				return -1;
			}
			memcpy(val, start, s-start);
			val[s-start] = 0;
		}
		evalval = eval(tf, val, KEEP_NODES);
		if(!evalval) {
			free(key);
			free(val);
			return -1;
		}
		if(vardb_set(args, key, evalval, NULL) < 0) {
			free(key);
			free(evalval);
			free(val);
			return -1;
		}
		free(key);
		free(evalval);
		free(val);
		while(isspace(*s))
			s++;
		if(*s == '}')
			break;
		if(*s != ',')
			return SYNTAX_ERROR;
		s++;
		first_entry = 0;
	}
	while(isspace(*s))
		s++;
	if(*s != '}')
		return SYNTAX_ERROR;
	s++;
	while(isspace(*s))
		s++;
	return *s == 0 ? 0 : SYNTAX_ERROR;
}

static int execute_function(struct tupfile *tf, struct tup_function *fn, struct vardb *args,
			    struct tup_build_ctx *ctx, const char *filename, int inherit_reldir,
			    struct vardb *returns)
{
	struct tup_func_frame frame;
	struct buf b;
	int old_in_function_body = tf->in_function_body;
	struct tup_func_frame *old_frame = tf->func_frame;
	int rc;

	if(vardb_clone(&frame.args, args) < 0)
		return -1;
	if(!inherit_reldir || vardb_get(&frame.args, "reldir", strlen("reldir")) == NULL) {
		if(function_arg_set_reldir(tf, &frame.args) < 0) {
			vardb_close(&frame.args);
			return -1;
		}
	}
	if(!inherit_reldir || vardb_get(&frame.args, "brdir", strlen("brdir")) == NULL) {
		if(function_arg_set_brdir(&frame.args) < 0) {
			vardb_close(&frame.args);
			return -1;
		}
	}
	if(vardb_init(&frame.vars) < 0) {
		vardb_close(&frame.args);
		return -1;
	}
	if(vardb_init(&frame.returns) < 0) {
		vardb_close(&frame.vars);
		vardb_close(&frame.args);
		return -1;
	}
	RB_INIT(&frame.bang_root);
	LIST_INIT(&frame.bin_list);
	frame.parent = tf->func_frame;
	frame.build_ctx = ctx ? ctx : (old_frame ? old_frame->build_ctx : NULL);
	frame.has_return = 0;

	b.s = strdup(fn->body);
	if(!b.s) {
		parser_error(tf, "strdup");
		vardb_close(&frame.args);
		vardb_close(&frame.vars);
		return -1;
	}
	b.len = strlen(b.s);

	tf->func_frame = &frame;
	tf->in_function_body = 1;
	rc = parse_tupfile(tf, &b, filename);
	tf->in_function_body = old_in_function_body;
	tf->func_frame = old_frame;

	if(rc >= 0 && returns && frame.has_return) {
		if(vardb_clone(returns, &frame.returns) < 0)
			rc = -1;
	}
	free_bang_tree(&frame.bang_root);
	bin_list_del(&frame.bin_list);
	vardb_close(&frame.returns);
	vardb_close(&frame.vars);
	vardb_close(&frame.args);
	free(b.s);
	return rc;
}

static int load_function_file(struct tupfile *tf, const char *file, struct tup_function_registry *reg,
			      struct tup_entry **oldtent, int *old_dfd, struct tup_entry **loadedtent)
{
	struct buf incb;
	int fd = -1;
	struct pel_group pg;
	struct path_element *pel = NULL;
	struct tup_entry *tent = NULL;
	tupid_t newdt;
	struct tup_entry *srctent = NULL;
	struct tup_entry *newtent;
	struct estring root_prefix;
	char *normalized_file = NULL;
	const char *resolved_file = file;
	int rc = -1;

	*oldtent = tf->curtent;
	*old_dfd = tf->cur_dfd;
	newdt = tf->curtent->tnode.tupid;
	estring_init(&root_prefix);
	if(strncmp(file, "//", 2) == 0) {
		const char *pkg = file + 2;
		struct tup_entry *current_src = variant_tent_to_srctent(tf->curtent);
		size_t prefix_len;
		size_t pkg_len = strlen(pkg);
		size_t suffix_len;
		const char *suffix;

		if(get_relative_dir(NULL, &root_prefix, current_src->tnode.tupid, DOT_DT) < 0)
			goto out;
		prefix_len = root_prefix.len;
		if(pkg_len == 0) {
			suffix = "Tupfile";
			suffix_len = strlen(suffix);
		} else if(pkg[pkg_len-1] == '/') {
			suffix = pkg;
			suffix_len = pkg_len + strlen("Tupfile");
		} else {
			suffix = pkg;
			suffix_len = pkg_len + strlen("/Tupfile");
		}
		normalized_file = malloc((strcmp(root_prefix.s, ".") == 0 ? 0 : prefix_len + 1) + suffix_len + 1);
		if(!normalized_file) {
			perror("malloc");
			goto out;
		}
		if(strcmp(root_prefix.s, ".") == 0) {
			if(pkg_len == 0) {
				snprintf(normalized_file, suffix_len + 1, "Tupfile");
			} else if(pkg[pkg_len-1] == '/') {
				snprintf(normalized_file, suffix_len + 1, "%sTupfile", pkg);
			} else {
				snprintf(normalized_file, suffix_len + 1, "%s/Tupfile", pkg);
			}
		} else if(pkg_len == 0) {
			snprintf(normalized_file, prefix_len + 1 + suffix_len + 1, "%s/Tupfile", root_prefix.s);
		} else if(pkg[pkg_len-1] == '/') {
			snprintf(normalized_file, prefix_len + 1 + suffix_len + 1, "%s/%sTupfile", root_prefix.s, pkg);
		} else {
			snprintf(normalized_file, prefix_len + 1 + suffix_len + 1, "%s/%s/Tupfile", root_prefix.s, pkg);
		}
		resolved_file = normalized_file;
	}
	if(get_path_elements(resolved_file, &pg) < 0)
		goto out;
	if(pg.pg_flags & PG_HIDDEN) {
		fprintf(tf->f, "tup error: Unable to call function from file with hidden path element.\n");
		goto out_pg;
	}
	newdt = find_dir_tupid_dt_pg(newdt, &pg, &pel, SOTGV_NO_GHOST, 0);
	if(newdt <= 0 || !pel) {
		fprintf(tf->f, "tup error: Unable to resolve function file '%s'.\n", file);
		goto out_pg;
	}
	newtent = tup_entry_get(newdt);
	if(tup_entry_variant(newtent) != tf->variant) {
		fprintf(tf->f, "tup error: Unable to call file '%s' outside the variant tree.\n", file);
		goto out_pg;
	}
	tf->curtent = newtent;
	if(variant_get_srctent(tf->variant, newtent, &srctent) < 0)
		goto out_pg;
	if(!srctent)
		srctent = tf->curtent;
	if(tup_db_select_tent_part(srctent, pel->path, pel->len, &tent) < 0 || !tent) {
		fprintf(tf->f, "tup error: Unable to find function file '%s'.\n", file);
		goto out_pel;
	}
	*loadedtent = newtent;
	tf->cur_dfd = tup_entry_openat(tf->root_fd, tent->parent);
	if(tf->cur_dfd < 0) {
		parser_error(tf, file);
		goto out_pel;
	}
	fd = parser_entry_open(tf, tent);
	if(fd < 0)
		goto out_dfd;
	if(fslurp_null(fd, &incb) < 0)
		goto out_fd;
	tf->function_registry = reg;
	if(scan_tupfile_functions(tf, &incb, file) < 0)
		goto out_buf;
	rc = 0;
out_buf:
	free(incb.s);
out_fd:
	if(fd >= 0)
		close(fd);
out_dfd:
	if(rc < 0 && tf->cur_dfd >= 0)
		close(tf->cur_dfd);
out_pel:
	free_pel(pel);
out_pg:
	del_pel_group(&pg);
out:
	free(root_prefix.s);
	free(normalized_file);
	return rc;
}

static void restore_loaded_function_file(struct tupfile *tf, struct tup_entry *oldtent, int old_dfd)
{
	if(tf->cur_dfd >= 0 && tf->cur_dfd != old_dfd)
		close(tf->cur_dfd);
	tf->curtent = oldtent;
	tf->cur_dfd = old_dfd;
}

static int invoke_function_path(struct tupfile *tf, const char *path, const char *fname,
				struct vardb *args, struct tup_build_ctx *ctx, int inherit_reldir,
				struct vardb *returns)
{
	struct string_tree *st;
	struct tup_function *fn;
	int rc;
	struct tup_entry *oldtent = NULL;
	int old_dfd = -1;
	struct tup_entry *loadedtent = NULL;
	struct tup_function_registry *old_registry = tf->function_registry;
	struct tup_function_registry *tmp_registry = NULL;

	tmp_registry = malloc(sizeof *tmp_registry);
	if(!tmp_registry) {
		perror("malloc");
		return -1;
	}
	function_registry_init(tmp_registry);
	if(load_function_file(tf, path, tmp_registry, &oldtent, &old_dfd, &loadedtent) < 0) {
		free_function_registry(tmp_registry);
		tf->function_registry = old_registry;
		return -1;
	}

	st = tf->function_registry ? string_tree_search(&tf->function_registry->root, fname, strlen(fname)) : NULL;
	if(!st) {
		fprintf(tf->f, "tup error: Unable to find function '%s'.\n", fname);
		rc = -1;
		goto out;
	}
	fn = container_of(st, struct tup_function, st);
	rc = execute_function(tf, fn, args, ctx, fname, inherit_reldir, returns);
out:
	restore_loaded_function_file(tf, oldtent, old_dfd);
	tf->function_registry = old_registry;
	free_function_registry(tmp_registry);
	return rc;
}

static int parse_function_invoke(struct tupfile *tf, char *line, int lno, int inherit_reldir)
{
	char *path = NULL;
	char *fname = NULL;
	char *argspec;
	char *endparen;
	struct vardb args;
	int rc;

	while(isspace(*line))
		line++;
	if(*line == '"') {
		const char *tmp = line;
		char *orig = line;
		rc = parse_quoted_string(&tmp, &path);
		if(rc < 0)
			return rc;
		line = orig + (tmp - orig);
		while(isspace(*line))
			line++;
	}
	fname = line;
	while(*line && !isspace(*line) && *line != '(')
		line++;
	if(*line == 0)
		return SYNTAX_ERROR;
	if(*line == '(') {
		*line = 0;
		argspec = line + 1;
	} else if(isspace(*line)) {
		*line = 0;
		line++;
		while(isspace(*line))
			line++;
		if(*line != '(')
			return SYNTAX_ERROR;
		argspec = line + 1;
	} else {
		return SYNTAX_ERROR;
	}
	endparen = strrchr(argspec, ')');
	if(!endparen)
		return SYNTAX_ERROR;
	*endparen = 0;
		if(parse_arg_map(tf, argspec, &args, 1) < 0)
		return -1;
	if(path) {
		rc = invoke_function_path(tf, path, fname, &args, NULL, inherit_reldir, NULL);
	} else {
		struct string_tree *st;
		struct tup_function *fn;

		st = tf->function_registry ? string_tree_search(&tf->function_registry->root, fname, strlen(fname)) : NULL;
		if(!st) {
			fprintf(tf->f, "tup error: Unable to find function '%s'.\n", fname);
			rc = -1;
		} else {
			fn = container_of(st, struct tup_function, st);
			rc = execute_function(tf, fn, &args, NULL, fname, inherit_reldir, NULL);
		}
	}
	vardb_close(&args);
	free(path);
	if(rc < 0)
		fprintf(tf->f, "tup error: Error invoking function on line %i.\n", lno);
	return rc;
}

static int parse_fbind(struct tupfile *tf, char *line, int lno)
{
	char *invoke;
	struct vardb bindings = {{0}, 0};
	struct vardb returns = {{0}, 0};
	int rc;
	int invoke_rc;
	int inherit_reldir;

	invoke = strstr(line, ":=");
	if(!invoke)
		return SYNTAX_ERROR;
	*invoke = 0;
	invoke += 2;

	while(isspace((unsigned char)*invoke))
		invoke++;
	if(strncmp(invoke, "dist", 4) == 0 ||
	   strncmp(invoke, "dest", 4) == 0) {
		char *var;
		char *spec;

		invoke_rc = parse_simple_fbind_var(line, &var);
		if(invoke_rc < 0)
			return invoke_rc;
		if(parse_dist_block(tf, invoke, &spec) < 0) {
			free(var);
			fprintf(tf->f, "tup error: Error invoking function on line %i.\n", lno);
			return -1;
		}
		invoke_rc = set_function_or_lua_var(tf, var, spec);
		free(spec);
		free(var);
		if(invoke_rc < 0)
			fprintf(tf->f, "tup error: Error invoking function on line %i.\n", lno);
		return invoke_rc;
	}
	if(strncmp(invoke, "materialize", 11) == 0) {
		char *var = NULL;
		char *spec_expr = NULL;
		char *root_expr = NULL;
		char *spec = NULL;
		char *root = NULL;
		char *root_ref = NULL;
		struct tup_entry *root_tent = NULL;
		struct tup_build_ctx *build_ctx = tf->func_frame ? tf->func_frame->build_ctx : NULL;
		struct name_list_entry *old_last_output = NULL;
		int root_from_caller_root = 0;

		invoke_rc = parse_simple_fbind_var(line, &var);
		if(invoke_rc < 0)
			return invoke_rc;
		invoke_rc = parse_materialize_args(tf, invoke, &spec_expr, &root_expr,
						    &root_from_caller_root);
		if(invoke_rc < 0)
			goto out_materialize;
		spec = eval(tf, spec_expr, KEEP_NODES);
		if(!spec) {
			invoke_rc = -1;
			goto out_materialize;
		}
		root = eval(tf, root_expr, EXPAND_NODES_SRC);
		if(!root) {
			invoke_rc = -1;
			goto out_materialize;
		}
		if(root[0] == 0) {
			fprintf(tf->f, "tup error: materialize requires a non-empty target directory.\n");
			invoke_rc = -1;
			goto out_materialize;
		}
		{
			char *canon = NULL;
			if(canonicalize_path_simple(root, &canon) < 0) {
				invoke_rc = -1;
				goto out_materialize;
			}
			free(root);
			root = canon;
		}
		if(build_ctx)
			old_last_output = TAILQ_LAST(&build_ctx->outputs.entries, name_list_entry_head);
		if(tupbuild_materialize_dist(tf, build_ctx, NULL, root, spec, root_from_caller_root) < 0) {
			invoke_rc = -1;
			goto out_materialize;
		}
		if(build_ctx_add_outputs_as_order_only_inputs(tf, build_ctx, old_last_output) < 0) {
			invoke_rc = -1;
			goto out_materialize;
		}
		if(resolve_materialized_dir(tf, root, root_from_caller_root, &root_tent) < 0) {
			invoke_rc = -1;
			goto out_materialize;
		}
		root_ref = malloc(32);
		if(!root_ref) {
			perror("malloc");
			invoke_rc = -1;
			goto out_materialize;
		}
		snprintf(root_ref, 31, "%%%llit", root_tent->tnode.tupid);
		root_ref[31] = 0;
		invoke_rc = set_function_or_lua_var(tf, var, root_ref);

out_materialize:
		free(root_ref);
		free(root);
		free(spec);
		free(root_expr);
		free(spec_expr);
		free(var);
		if(invoke_rc < 0)
			fprintf(tf->f, "tup error: Error invoking function on line %i.\n", lno);
		return invoke_rc;
	}

	rc = parse_fbind_map(line, &bindings);
	if(rc < 0) {
		vardb_close(&bindings);
		return rc;
	}
	if(vardb_init(&returns) < 0) {
		vardb_close(&bindings);
		return -1;
	}

	while(isspace(*invoke))
		invoke++;
	if(strncmp(invoke, "call ", 5) == 0) {
		inherit_reldir = 1;
		invoke += 5;
	} else if(strncmp(invoke, "spawn ", 6) == 0) {
		inherit_reldir = 0;
		invoke += 6;
	} else {
		vardb_close(&returns);
		vardb_close(&bindings);
		return SYNTAX_ERROR;
	}

	{
		char *path = NULL;
		char *fname = NULL;
		char *argspec;
		char *endparen;
		struct vardb args;

		while(isspace(*invoke))
			invoke++;
		if(*invoke == '"') {
			const char *tmp = invoke;
			char *orig = invoke;
			invoke_rc = parse_quoted_string(&tmp, &path);
			if(invoke_rc < 0) {
				vardb_close(&returns);
				vardb_close(&bindings);
				return invoke_rc;
			}
			invoke = orig + (tmp - orig);
			while(isspace(*invoke))
				invoke++;
		}
		fname = invoke;
		while(*invoke && !isspace(*invoke) && *invoke != '(')
			invoke++;
		if(*invoke == 0) {
			invoke_rc = SYNTAX_ERROR;
			goto out_invoke;
		}
		if(*invoke == '(') {
			*invoke = 0;
			argspec = invoke + 1;
		} else if(isspace(*invoke)) {
			*invoke = 0;
			invoke++;
			while(isspace(*invoke))
				invoke++;
			if(*invoke != '(') {
				invoke_rc = SYNTAX_ERROR;
				goto out_invoke;
			}
			argspec = invoke + 1;
		} else {
			invoke_rc = SYNTAX_ERROR;
			goto out_invoke;
		}
		endparen = strrchr(argspec, ')');
		if(!endparen) {
			invoke_rc = SYNTAX_ERROR;
			goto out_invoke;
		}
		*endparen = 0;
		if(parse_arg_map(tf, argspec, &args, 1) < 0) {
			invoke_rc = -1;
			goto out_invoke;
		}
		if(path) {
			invoke_rc = invoke_function_path(tf, path, fname, &args, NULL, inherit_reldir, &returns);
		} else {
			struct string_tree *st;
			struct tup_function *fn;

			st = tf->function_registry ? string_tree_search(&tf->function_registry->root, fname, strlen(fname)) : NULL;
			if(!st) {
				fprintf(tf->f, "tup error: Unable to find function '%s'.\n", fname);
				invoke_rc = -1;
			} else {
				fn = container_of(st, struct tup_function, st);
				invoke_rc = execute_function(tf, fn, &args, NULL, fname, inherit_reldir, &returns);
			}
		}
		vardb_close(&args);
out_invoke:
		free(path);
	}

	if(invoke_rc >= 0 && bindings.count > 0 && returns.count == 0) {
		fprintf(tf->f, "tup error: fbind requires the invoked function to return values.\n");
		invoke_rc = -1;
	}
	if(invoke_rc >= 0)
		invoke_rc = assign_fbind_vars(tf, &bindings, &returns);

	vardb_close(&returns);
	vardb_close(&bindings);
	if(invoke_rc < 0)
		fprintf(tf->f, "tup error: Error invoking function on line %i.\n", lno);
	return invoke_rc;
}

static int tupbuild_find_build(struct tupbuild_file *tb, const char *name)
{
	int x;
	for(x=0; x<tb->num_builds; x++) {
		if(strcmp(tb->builds[x].name, name) == 0)
			return x;
	}
	return -1;
}

static int tupbuild_set_arg(struct vardb *args, const char *key, const char *value)
{
	if(vardb_get(args, key, strlen(key)) != NULL)
		return -1;
	return vardb_set(args, key, value, NULL);
}

static int tupbuild_parse_from_expr(const char *value, char **build_name, char **ret_name)
{
	const char *s = value;

	*build_name = NULL;
	*ret_name = NULL;
	while(isspace((unsigned char)*s))
		s++;
	if(strncmp(s, "$(from", 6) != 0)
		return 0;
	s += 6;
	while(isspace((unsigned char)*s))
		s++;
	if(parse_quoted_string(&s, build_name) < 0)
		return -1;
	while(isspace((unsigned char)*s))
		s++;
	if(parse_quoted_string(&s, ret_name) < 0) {
		free(*build_name);
		*build_name = NULL;
		return -1;
	}
	while(isspace((unsigned char)*s))
		s++;
	if(*s != ')') {
		free(*build_name);
		free(*ret_name);
		*build_name = NULL;
		*ret_name = NULL;
		return -1;
	}
	s++;
	while(isspace((unsigned char)*s))
		s++;
	if(*s != 0) {
		free(*build_name);
		free(*ret_name);
		*build_name = NULL;
		*ret_name = NULL;
		return -1;
	}
	return 1;
}

static int tupbuild_resolve_arg_value(struct tupfile *tf, struct tupbuild_file *tb,
				      struct tupbuild_build *build, struct vardb *build_returns,
				      int *returns_valid, const char *value, const char **resolved)
{
	char *dep_build = NULL;
	char *ret_name = NULL;
	int parse_rc;
	int depidx;
	int y;

	*resolved = value;
	parse_rc = tupbuild_parse_from_expr(value, &dep_build, &ret_name);
	if(parse_rc <= 0)
		return parse_rc;
	depidx = tupbuild_find_build(tb, dep_build);
	if(depidx < 0) {
		fprintf(tf->f, "tup error: Build '%s' references unknown build '%s' in $(from ...).\n",
			build->name, dep_build);
		goto fail;
	}
	for(y=0; y<build->num_depends; y++) {
		if(strcmp(build->depends[y].build, dep_build) == 0)
			break;
	}
	if(y >= build->num_depends) {
		fprintf(tf->f, "tup error: Build '%s' can only use $(from ...) for declared dependencies. Missing '%s'.\n",
			build->name, dep_build);
		goto fail;
	}
	if(!returns_valid[depidx]) {
		fprintf(tf->f, "tup error: Build '%s' referenced dependency return from '%s' before it was available.\n",
			build->name, dep_build);
		goto fail;
	}
	{
		struct var_entry *ret = vardb_get(&build_returns[depidx], ret_name, strlen(ret_name));
		if(!ret || !ret->value) {
			fprintf(tf->f, "tup error: Build '%s' requested missing return '%s' from dependency '%s'.\n",
				build->name, ret_name, dep_build);
			goto fail;
		}
		*resolved = ret->value;
	}
	free(dep_build);
	free(ret_name);
	return 1;
fail:
	free(dep_build);
	free(ret_name);
	return -1;
}

static int tupbuild_build_stamp_path(const char *builddir, const char *name, char **out)
{
	const char *prefix = "__tupbuild_";
	const char *suffix = ".stamp";
	int len;

	if(strcmp(builddir, ".") == 0 || builddir[0] == 0) {
		len = strlen(prefix) + strlen(name) + strlen(suffix);
		*out = malloc(len + 1);
		if(!*out) {
			perror("malloc");
			return -1;
		}
		snprintf(*out, len + 1, "%s%s%s", prefix, name, suffix);
	} else {
		int needs_sep = builddir[strlen(builddir)-1] != '/';
		len = strlen(builddir) + needs_sep + strlen(prefix) + strlen(name) + strlen(suffix);
		*out = malloc(len + 1);
		if(!*out) {
			perror("malloc");
			return -1;
		}
		snprintf(*out, len + 1, "%s%s%s%s%s",
			 builddir,
			 needs_sep ? "/" : "",
			 prefix,
			 name,
			 suffix);
	}
	return 0;
}

static int tupbuild_combine_builddir_reldir(const char *builddir, const char *reldir, char **out)
{
	struct pel_group pg;
	struct path_element *pel;
	char *tmp;
	int len = 0;
	int first = 1;

	tmp = malloc(strlen(builddir) + strlen(reldir) + 2);
	if(!tmp) {
		perror("malloc");
		return -1;
	}
	snprintf(tmp, strlen(builddir) + strlen(reldir) + 2, "%s/%s", builddir, reldir);
	if(get_path_elements(tmp, &pg) < 0) {
		free(tmp);
		return -1;
	}

	if(TAILQ_EMPTY(&pg.path_list)) {
		*out = strdup(".");
		if(!*out) {
			perror("strdup");
			free(tmp);
			return -1;
		}
		free(tmp);
		return 0;
	}

	TAILQ_FOREACH(pel, &pg.path_list, list) {
		len += pel->len;
		if(!first)
			len++;
		first = 0;
	}
	*out = malloc(len + 1);
	if(!*out) {
		perror("malloc");
		del_pel_group(&pg);
		free(tmp);
		return -1;
	}
	len = 0;
	first = 1;
	TAILQ_FOREACH(pel, &pg.path_list, list) {
		if(!first)
			(*out)[len++] = '/';
		memcpy(*out + len, pel->path, pel->len);
		len += pel->len;
		first = 0;
	}
	(*out)[len] = 0;
	del_pel_group(&pg);
	free(tmp);
	return 0;
}

static int tupbuild_resolve_builddir(struct tupfile *tf, const char *builddir, struct tup_entry **tent)
{
	struct path_list *pl;
	struct path_list_head plist;
	char *tmp;
	int rc = -1;

	TAILQ_INIT(&plist);
	tmp = malloc(strlen(builddir) + sizeof("/__tupbuild"));
	if(!tmp) {
		perror("malloc");
		return -1;
	}
	if(strcmp(builddir, ".") == 0 || builddir[0] == 0)
		snprintf(tmp, strlen(builddir) + sizeof("/__tupbuild"), "__tupbuild");
	else
		snprintf(tmp, strlen(builddir) + sizeof("/__tupbuild"), "%s/%s", builddir, "__tupbuild");

	pl = new_pl(tf, tmp, -1, NULL, 1);
	free(tmp);
	if(!pl)
		return -1;
	TAILQ_INSERT_TAIL(&plist, pl, list);
	if(path_list_fill_dt_pel(tf, pl, tf->tent->tnode.tupid, 1) < 0)
		goto out;
	if(tup_entry_add(pl->dt, tent) < 0)
		goto out;
	rc = 0;
out:
	free_path_list(&plist);
	return rc;
}

static int tupbuild_clone_inputs(struct name_list *dst, struct name_list *src)
{
	struct name_list_entry *nle;

	TAILQ_FOREACH(nle, &src->entries, list) {
		struct name_list_entry *copy;

		copy = malloc(sizeof *copy);
		if(!copy) {
			perror("malloc");
			return -1;
		}
		memcpy(copy, nle, sizeof *copy);
		copy->path = strdup(nle->path);
		if(!copy->path) {
			perror("strdup");
			free(copy);
			return -1;
		}
		add_name_list_entry(dst, copy);
	}
	return 0;
}

static int tupbuild_create_stamp_rule(struct tupfile *tf, const char *stamp_path, struct name_list *deps)
{
	struct rule r;
	int rc;

	init_rule(&r);
	r.command = "touch %o";
	r.command_len = strlen(r.command);
	r.empty_input = 1;
	if(get_path_list(tf, stamp_path, &r.outputs, NULL) < 0)
		return -1;
	if(tupbuild_clone_inputs(&r.order_only_inputs, deps) < 0) {
		free_path_list(&r.outputs);
		delete_name_list(&r.order_only_inputs);
		return -1;
	}
	rc = execute_rule(tf, &r, NULL);
	delete_name_list(&r.order_only_inputs);
	free_path_list(&r.outputs);
	free_path_list(&r.order_only_input_paths);
	free_path_list(&r.extra_outputs);
	free_path_list(&r.bang_extra_outputs);
	return rc;
}

static int tupbuild_create_copy_rule(struct tupfile *tf, struct tup_build_ctx *ctx,
				     const char *src, const char *dest,
				     int dest_from_caller_root)
{
	struct rule r;
	struct path_list *pl;
	int rc;

	init_rule(&r);
	r.command = "mkdir -p \"\\$(dirname %o)\" && cp %f %o";
	r.command_len = strlen(r.command);
	if(get_path_list(tf, src, &r.order_only_input_paths, NULL) < 0)
		return -1;
	if(path_list_to_nl(tf, &r.order_only_input_paths, &r.inputs, NULL, 0) < 0) {
		free_path_list(&r.order_only_input_paths);
		return -1;
	}
	free_path_list(&r.order_only_input_paths);
	TAILQ_INIT(&r.order_only_input_paths);
	if(get_path_list(tf, dest, &r.outputs, NULL) < 0) {
		delete_name_list(&r.inputs);
		return -1;
	}
	if(dest_from_caller_root) {
		TAILQ_FOREACH(pl, &r.outputs, list) {
			pl->tent_relative = 0;
		}
	}
	rc = execute_rule(tf, &r, ctx ? &ctx->outputs : NULL);
	delete_name_list(&r.inputs);
	delete_name_list(&r.order_only_inputs);
	delete_name_list(&r.bang_oo_inputs);
	free_path_list(&r.order_only_input_paths);
	free_path_list(&r.outputs);
	free_path_list(&r.extra_outputs);
	free_path_list(&r.bang_extra_outputs);
	return rc;
}

static int tupbuild_dist_output_path(const char *root, const char *dest, char **out)
{
	char *joined;
	char *canon;
	size_t len;

	while(*dest == '/')
		dest++;
	len = strlen(root) + strlen(dest) + 2;
	joined = malloc(len);
	if(!joined) {
		perror("malloc");
		return -1;
	}
	snprintf(joined, len, "%s/%s", root, dest);
	if(canonicalize_path_simple(joined, &canon) < 0) {
		free(joined);
		return -1;
	}
	free(joined);
	*out = canon;
	return 0;
}

static int tupbuild_materialize_dist(struct tupfile *tf, struct tup_build_ctx *ctx,
				     const char *build_name, const char *root,
				     const char *spec, int root_from_caller_root)
{
	struct dist_manifest dm;
	int x;

	(void)build_name;
	if(dist_parse_manifest(tf, spec, &dm) < 0)
		return -1;
	for(x=0; x<dm.num_entries; x++) {
		char *dest;

		if(tupbuild_dist_output_path(root, dm.entries[x].dest, &dest) < 0) {
			dist_manifest_free(&dm);
			return -1;
		}
		if(tupbuild_create_copy_rule(tf, ctx, dm.entries[x].src, dest,
					 root_from_caller_root) < 0) {
			free(dest);
			dist_manifest_free(&dm);
			return -1;
		}
		free(dest);
	}
	dist_manifest_free(&dm);
	return 0;
}

static int resolve_materialized_dir(struct tupfile *tf, const char *path,
				    int root_from_caller_root, struct tup_entry **tent)
{
	struct path_list_head plist;
	struct path_list *pl;
	struct tup_entry *dtent;
	tupid_t base;
	int rc = -1;

	if(strcmp(path, ".") == 0 || path[0] == 0) {
		*tent = (root_from_caller_root || !tf->func_frame) ? tf->tent : tf->curtent;
		return 0;
	}

	TAILQ_INIT(&plist);
	pl = new_pl(tf, path, -1, NULL, 1);
	if(!pl)
		return -1;
	if(root_from_caller_root)
		pl->tent_relative = 0;
	TAILQ_INSERT_TAIL(&plist, pl, list);
	base = pl->tent_relative ? tf->curtent->tnode.tupid : tf->tent->tnode.tupid;
	if(path_list_fill_dt_pel(tf, pl, base, 1) < 0)
		goto out;
	if(tup_entry_add(pl->dt, &dtent) < 0)
		goto out;
	if(pl->pel->len == 2 && strncmp(pl->pel->path, "..", 2) == 0) {
		if(!dtent->parent) {
			fprintf(tf->f, "tup error: materialize target '%s' points beyond the tup hierarchy.\n", path);
			goto out;
		}
		*tent = dtent->parent;
	} else {
		if(tup_db_select_tent_part(dtent, pl->pel->path, pl->pel->len, tent) < 0)
			goto out;
		if(!*tent) {
			fprintf(tf->f, "tup error: materialize target '%s' did not resolve to a directory.\n", path);
			goto out;
		}
	}
	if((*tent)->type != TUP_NODE_DIR && (*tent)->type != TUP_NODE_GENERATED_DIR) {
		fprintf(tf->f, "tup error: materialize target '%s' resolved to a '%s', not a directory.\n",
			path, tup_db_type((*tent)->type));
		goto out;
	}
	rc = 0;
out:
	free_path_list(&plist);
	return rc;
}

static int parse_tupbuild(struct tupfile *tf, struct buf *b, const char *filename)
{
	struct tupbuild_file tb;
	char *err = NULL;
	int *indegree = NULL;
	int *order = NULL;
	int *queued = NULL;
	char **stamp_paths = NULL;
	struct vardb *build_returns = NULL;
	int *returns_valid = NULL;
	int strict_rc = -1;
	int x;
	int oi = 0;

	memset(&tb, 0, sizeof tb);
	if(tupbuild_parse(filename, b->s, b->len, &tb, &err) < 0) {
		if(err) {
			fprintf(tf->f, "%s\n", err);
			free(err);
		}
		return -1;
	}
	if(tb.auto_compiledb)
		parser_auto_compiledb = 1;

	indegree = calloc(tb.num_builds, sizeof *indegree);
	order = calloc(tb.num_builds, sizeof *order);
	queued = calloc(tb.num_builds, sizeof *queued);
	stamp_paths = calloc(tb.num_builds, sizeof *stamp_paths);
	build_returns = calloc(tb.num_builds, sizeof *build_returns);
	returns_valid = calloc(tb.num_builds, sizeof *returns_valid);
	if(!indegree || !order || !queued || !stamp_paths || !build_returns || !returns_valid) {
		parser_error(tf, "calloc");
		goto out;
	}

	for(x=0; x<tb.num_builds; x++) {
		int y;
		for(y=x+1; y<tb.num_builds; y++) {
			if(strcmp(tb.builds[x].name, tb.builds[y].name) == 0) {
				fprintf(tf->f, "tup error: Duplicate TupBuild.yaml build name '%s'.\n", tb.builds[x].name);
				goto out;
			}
			if(strcmp(tb.builds[x].builddir, tb.builds[y].builddir) == 0) {
				fprintf(tf->f, "tup error: Duplicate TupBuild.yaml builddir '%s'.\n", tb.builds[x].builddir);
				goto out;
			}
		}
		for(y=0; y<tb.builds[x].num_depends; y++) {
			int depidx;
			int z;
			depidx = tupbuild_find_build(&tb, tb.builds[x].depends[y].build);
			if(depidx < 0) {
				fprintf(tf->f, "tup error: Build '%s' depends on unknown build '%s'.\n",
					tb.builds[x].name, tb.builds[x].depends[y].build);
				goto out;
			}
			indegree[x]++;
			for(z=y+1; z<tb.builds[x].num_depends; z++) {
				if(strcmp(tb.builds[x].depends[y].as, tb.builds[x].depends[z].as) == 0) {
					fprintf(tf->f, "tup error: Build '%s' uses duplicate dependency alias '%s'.\n",
						tb.builds[x].name, tb.builds[x].depends[y].as);
					goto out;
				}
			}
		}
	}

	while(oi < tb.num_builds) {
		int progress = 0;
		for(x=0; x<tb.num_builds; x++) {
			int y;
			if(queued[x] || indegree[x] != 0)
				continue;
			queued[x] = 1;
			order[oi++] = x;
			progress = 1;
			for(y=0; y<tb.num_builds; y++) {
				int z;
				for(z=0; z<tb.builds[y].num_depends; z++) {
					if(strcmp(tb.builds[y].depends[z].build, tb.builds[x].name) == 0)
						indegree[y]--;
				}
			}
		}
		if(!progress) {
			fprintf(tf->f, "tup error: TupBuild.yaml contains a dependency cycle.\n");
			goto out;
		}
	}

	for(oi=0; oi<tb.num_builds; oi++) {
		struct tupbuild_build *build;
		struct vardb args;
		struct vardb returns;
		struct tup_build_ctx ctx;
		int idx = order[oi];
		int y;

		build = &tb.builds[idx];
		if(vardb_init(&args) < 0)
			goto out;
		if(vardb_init(&returns) < 0) {
			vardb_close(&args);
			goto out;
		}
		build_ctx_init(&ctx);
		ctx.strict = tb.strict;
		ctx.caller_base_tent = tf->curtent;
		if(build_ctx_add_caller_path_var(&ctx, "builddir") < 0) {
			vardb_close(&args);
			vardb_close(&returns);
			goto out;
		}
		if(build_ctx_add_caller_path_var(&ctx, "brdir") < 0) {
			vardb_close(&args);
			vardb_close(&returns);
			goto out;
		}

		for(y=0; y<tb.num_global_args; y++) {
			const char *resolved = NULL;
			if(strcmp(tb.global_args[y].key, "builddir") == 0 || strcmp(tb.global_args[y].key, "brdir") == 0) {
				fprintf(tf->f, "tup error: globalArgs cannot override reserved variable '%s'.\n", tb.global_args[y].key);
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(tupbuild_resolve_arg_value(tf, &tb, build, build_returns, returns_valid, tb.global_args[y].value, &resolved) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(tupbuild_set_arg(&args, tb.global_args[y].key, resolved) < 0) {
				fprintf(tf->f, "tup error: Duplicate argument '%s' in globalArgs.\n", tb.global_args[y].key);
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
		}
		for(y=0; y<build->num_args; y++) {
			const char *resolved = NULL;
			if(strcmp(build->args[y].key, "builddir") == 0 || strcmp(build->args[y].key, "brdir") == 0) {
				fprintf(tf->f, "tup error: Build '%s' cannot override reserved variable '%s'.\n", build->name, build->args[y].key);
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(tupbuild_resolve_arg_value(tf, &tb, build, build_returns, returns_valid, build->args[y].value, &resolved) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(vardb_set(&args, build->args[y].key, resolved, NULL) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
		}
		if(build->profile) {
			if(strcmp(build->profile, "builddir") == 0 || strcmp(build->profile, "brdir") == 0) {
				fprintf(tf->f, "tup error: Build '%s' cannot use reserved profile name '%s'.\n",
					build->name, build->profile);
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(vardb_set(&args, build->profile, "true", NULL) < 0) {
				fprintf(tf->f, "tup error: Build '%s' has duplicate profile argument '%s'.\n",
					build->name, build->profile);
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
		}
		for(y=0; y<build->num_depends; y++) {
			int depidx;
			depidx = tupbuild_find_build(&tb, build->depends[y].build);
			if(strcmp(build->depends[y].as, "builddir") == 0 ||
			   strcmp(build->depends[y].as, "brdir") == 0 ||
			   vardb_get(&args, build->depends[y].as, strlen(build->depends[y].as)) != NULL) {
				fprintf(tf->f, "tup error: Build '%s' cannot override dependency alias '%s'.\n",
					build->name, build->depends[y].as);
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(build_ctx_add_caller_path_var(&ctx, build->depends[y].as) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(vardb_set(&args, build->depends[y].as, tb.builds[depidx].builddir, NULL) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
			if(get_path_list(tf, stamp_paths[depidx], &ctx.order_only_input_paths, NULL) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				goto out;
			}
		}
		if(vardb_set(&args, "builddir", build->builddir, NULL) < 0) {
			vardb_close(&args);
			vardb_close(&returns);
			goto out;
		}
		if(build->tupfile && build->function) {
			if(invoke_function_path(tf, build->tupfile, build->function, &args, &ctx, 0, &returns) < 0) {
				vardb_close(&args);
				vardb_close(&returns);
				build_ctx_close(&ctx);
				goto out;
			}
			for(y=0; y<build->num_dists; y++) {
				struct var_entry *ret;

				ret = vardb_get(&returns, build->dists[y].from_return, strlen(build->dists[y].from_return));
				if(!ret || !ret->value) {
					fprintf(tf->f, "tup error: Build '%s' requested dist return '%s', but the function did not provide it.\n",
						build->name, build->dists[y].from_return);
					vardb_close(&args);
					vardb_close(&returns);
					build_ctx_close(&ctx);
					goto out;
				}
				if(tupbuild_materialize_dist(tf, &ctx, build->name, build->dists[y].path,
							 ret->value, 0) < 0) {
					vardb_close(&args);
					vardb_close(&returns);
					build_ctx_close(&ctx);
					goto out;
				}
			}
		}
		if(tupbuild_build_stamp_path(build->builddir, build->name, &stamp_paths[idx]) < 0) {
			vardb_close(&args);
			vardb_close(&returns);
			build_ctx_close(&ctx);
			goto out;
		}
		if(tupbuild_create_stamp_rule(tf, stamp_paths[idx], &ctx.outputs) < 0) {
			vardb_close(&args);
			vardb_close(&returns);
			build_ctx_close(&ctx);
			goto out;
		}
		vardb_close(&build_returns[idx]);
		if(vardb_clone(&build_returns[idx], &returns) < 0) {
			vardb_close(&args);
			vardb_close(&returns);
			build_ctx_close(&ctx);
			goto out;
		}
		returns_valid[idx] = 1;
		vardb_close(&args);
		vardb_close(&returns);
		build_ctx_close(&ctx);
	}

	strict_rc = 0;
out:
	if(stamp_paths) {
		for(x=0; x<tb.num_builds; x++)
			free(stamp_paths[x]);
	}
	free(stamp_paths);
	free(indegree);
	free(order);
	free(queued);
	if(build_returns) {
		for(x=0; x<tb.num_builds; x++)
			vardb_close(&build_returns[x]);
	}
	free(build_returns);
	free(returns_valid);
	tupbuild_free(&tb);
	return strict_rc;
}

static int parse_bang_rule_internal(struct tupfile *tf, struct rule *r,
				    struct string_tree *st, struct name_list *nl)
{
	struct bang_rule *br;
	br = container_of(st, struct bang_rule, st);

	/* Add any order only inputs to the list */
	if(br->input) {
		struct path_list_head head;
		TAILQ_INIT(&head);
		if(get_path_list(tf, br->input, &head, NULL) < 0)
			return -1;
		if(path_list_to_nl(tf, &head, &r->bang_oo_inputs, nl, 0) < 0)
			return -1;
		free_path_list(&head);
	}

	/* The command gets replaced whole-sale */
	r->command = br->command;
	r->command_len = br->command_len;

	/* If the rule didn't specify any output pattern, use the one from the
	 * !-macro.
	 */
	if(TAILQ_EMPTY(&r->outputs)) {
		if(copy_path_list(tf, &r->outputs, &br->outputs) < 0)
			return -1;
	}

	/* Also include any extra outputs from the !-macro. These may specify
	 * additional outputs that the user of the !-macro doesn't know about
	 * (such as command side-effects).
	 */
	if(copy_path_list(tf, &r->bang_extra_outputs, &br->extra_outputs) < 0)
		return -1;
	return 0;
}

static int parse_empty_bang_rule(struct tupfile *tf, struct rule *r)
{
	struct string_tree *st;
	char empty[] = ".EMPTY";
	char tmp[r->command_len + sizeof(empty)];

	memcpy(tmp, r->command, r->command_len);
	memcpy(tmp + r->command_len, empty, sizeof(empty));

	st = string_tree_search(&tf->bang_root, tmp, sizeof(tmp) - 1);
	if(!st)
		return 1;
	return parse_bang_rule_internal(tf, r, st, NULL);
}

static int parse_bang_rule(struct tupfile *tf, struct rule *r,
			   struct name_list *nl, const char *ext, int extlen)
{
	struct string_tree *st = NULL;

	/* First try to find the extension-specific rule, and if not then use
	 * the general one. Eg: if the input is foo.c, then the extension is ".c",
	 * so try "!cc.c" first, then "!cc" second.
	 */
	if(ext) {
		char tmp[r->command_len + extlen + 2];
		snprintf(tmp, sizeof(tmp), "%.*s.%.*s", r->command_len, r->command, extlen, ext);
		st = string_tree_search(&tf->bang_root, tmp, sizeof(tmp) - 1);
	}
	if(!st) {
		st = find_bang_rule(tf, r->command, r->command_len);
		if(!st) {
			fprintf(tf->f, "tup error: Error finding bang variable: '%s'\n",
				r->command);
			return -1;
		}
	}
	return parse_bang_rule_internal(tf, r, st, nl);
}

static void free_bang_rule(struct string_entries *root, struct bang_rule *br)
{
	string_tree_remove(root, &br->st);

	if(br->value) {
		/* For regular macros */
		free(br->value);
	} else {
		/* For aliased macros */
		free(br->input);
		free(br->command);
	}
	free_path_list(&br->outputs);
	free_path_list(&br->extra_outputs);
	free(br);
}

static void free_bang_tree(struct string_entries *root)
{
	struct string_tree *st;

	while((st = RB_ROOT(root)) != NULL) {
		free_bang_rule(root, container_of(st, struct bang_rule, st));
	}
}

static int split_input_pattern(struct tupfile *tf, char *p, char **o_input,
			       char **o_cmd, int *o_cmdlen, char **o_output,
			       char **o_bin)
{
	char *input;
	char *cmd;
	char *output;
	char *bin = NULL;
	char *brace;
	char *ebrace = NULL;
	char *ie, *ce;
	const char *marker = "|>";

	input = p;
	while(isspace(*input))
		input++;
	p = strstr(p, marker);
	if(!p) {
		fprintf(tf->f, "tup error: Missing '|>' marker.\n");
		return -1;
	}
	if(input < p) {
		ie = p - 1;
		while(isspace(*ie) && ie > input)
			ie--;
		ie[1] = 0;
	} else {
		input = NULL;
	}
	p += 2;
	cmd = p;
	while(isspace(*cmd))
		cmd++;

	p = strstr(p, marker);
	if(!p) {
		fprintf(tf->f, "tup error: Missing second '%s' marker.\n", marker);
		return -1;
	}
	ce = p - 1;
	while(isspace(*ce) && ce > cmd)
		ce--;
	p += 2;
	output = p;
	while(isspace(*output))
		output++;
	ce[1] = 0;

	brace = output;
	while(1) {
		brace = strchr(brace, '{');
		if(!brace)
			break;
		ebrace = brace - 1;

		/* If the character before the '{' is not a space (and we're
		 * not the first character in the output string), then it's
		 * part of a filename.
		 */
		if(brace != output && !isspace(*ebrace)) {
			brace++;
			continue;
		}
		while(isspace(*ebrace) && ebrace > output)
			ebrace--;
		bin = brace + 1;

		brace = strchr(bin, '}');
		if(!brace) {
			fprintf(tf->f, "tup error: Missing '}' to finish bin.\n");
			return -1;
		}
		*brace = 0;
		ebrace[1] = 0;
		if(brace[1]) {
			fprintf(tf->f, "tup error: bin must be at the end of the output list.\n");
			return -1;
		}
		break;
	}

	*o_input = input;
	*o_cmd = cmd;
	*o_cmdlen = ce - cmd + 1;
	*o_output = output;
	*o_bin = bin;
	return 0;
}

static int parse_input_pattern(struct tupfile *tf, char *input_pattern,
			       struct name_list *inputs,
			       struct path_list_head *order_only_input_paths,
			       struct bin_head *bl,
			       int is_variant_copy)
{
	char *oosep;

	if(!input_pattern)
		return 0;

	oosep = strchr(input_pattern, '|');
	if(oosep) {
		char *p = oosep;
		*p = 0;
		while(p >= input_pattern && isspace(*p)) {
			*p = 0;
			p--;
		}
		oosep++;
		while(*oosep && isspace(*oosep)) {
			*oosep = 0;
			oosep++;
		}
		if(order_only_input_paths) {
			if(get_path_list(tf, oosep, order_only_input_paths, bl) < 0)
				return -1;
		}
	}
	if(inputs) {
		struct path_list_head plist;

		TAILQ_INIT(&plist);
		if(get_path_list(tf, input_pattern, &plist, bl) < 0)
			return -1;
		if(path_list_to_nl(tf, &plist, inputs, NULL, is_variant_copy) < 0)
			return -1;
		free_path_list(&plist);
	}
	return 0;
}

static int parse_output_pattern(struct tupfile *tf, char *output_pattern,
				struct path_list_head *outputs,
				struct path_list_head *extra_outputs)
{
	char *oosep;

	if(!output_pattern)
		return 0;

	oosep = strchr(output_pattern, '|');
	if(oosep) {
		char *p = oosep;
		*p = 0;
		while(p >= output_pattern && isspace(*p)) {
			*p = 0;
			p--;
		}
		oosep++;
		while(*oosep && isspace(*oosep)) {
			*oosep = 0;
			oosep++;
		}
		if(get_path_list(tf, oosep, extra_outputs, NULL) < 0)
			return -1;
	}
	if(get_path_list(tf, output_pattern, outputs, NULL) < 0)
		return -1;
	return 0;
}

static int make_name_list_unique(struct name_list *nl)
{
	struct name_list_entry *tmp;
	struct tent_entries root = TENT_ENTRIES_INITIALIZER;
	struct name_list_entry *nle;

	/* We only care about dupes in the inputs namelist, since the others
	 * are just added to the tupid_tree and aren't used in %-flags.
	 *
	 * Note that we need to prune duplicate inputs, but still maintain the
	 * order.
	 */
	TAILQ_FOREACH_SAFE(nle, &nl->entries, list, tmp) {
		if(!nle->tent)
			continue;
		if(tent_tree_search(&root, nle->tent) != NULL) {
			delete_name_list_entry(nl, nle);
		} else {
			if(tent_tree_add(&root, nle->tent) < 0)
				return -1;
		}
	}
	free_tent_tree(&root);
	return 0;
}

int execute_rule(struct tupfile *tf, struct rule *r, struct name_list *output_nl)
{
	struct name_list_entry *nle;
	int is_bang = 0;
	int foreach = 0;

	if(build_ctx_copy_order_only_inputs(tf, r) < 0)
		return -1;

	if(make_name_list_unique(&r->inputs) < 0)
		return -1;

	if(r->command[0] == '!') {
		struct string_tree *st;
		struct bang_rule *br;

		is_bang = 1;

		/* If we can't find the actual !-macro, it may be that there
		 * are only extension-specific macros, in which case the rule
		 * itself determines the foreach-ness.
		 */
		st = string_tree_search(&tf->bang_root, r->command, r->command_len);
		if(st) {
			br = container_of(st, struct bang_rule, st);
			foreach = br->foreach;
		}
	}
	/* Either the !-macro or the rule itself can list 'foreach'. There is no
	 * way to cancel a foreach in a !-macro.
	 */
	if(r->foreach)
		foreach = r->foreach;

	if(foreach) {
		struct name_list tmp_nl;
		struct name_list_entry tmp_nle;
		const char *old_command = NULL;
		int outputs_empty = 0;
		int old_command_len = 0;

		if(TAILQ_EMPTY(&r->outputs))
			outputs_empty = 1;

		/* For a foreach loop, iterate over each entry in the rule's
		 * namelist and do a shallow copy over into a single-entry
		 * temporary namelist. Note that we cheat by not actually
		 * allocating a separate nle, which is why we don't have to do
		 * a delete_name_list_entry for the temporary list and can just
		 * reinitialize the pointers using init_name_list.
		 */
		while(!TAILQ_EMPTY(&r->inputs.entries)) {
			const char *ext = NULL;
			int extlen = 0;

			nle = TAILQ_FIRST(&r->inputs.entries);

			init_name_list(&tmp_nl);
			memcpy(&tmp_nle, nle, sizeof(*nle));
			add_name_list_entry(&tmp_nl, &tmp_nle);
			if(tmp_nle.base &&
			   tmp_nle.extlessbaselen != tmp_nle.baselen) {
				ext = tmp_nle.base + tmp_nle.extlessbaselen + 1;
				extlen = tmp_nle.baselen - tmp_nle.extlessbaselen - 1;
			}
			if(is_bang) {
				/* parse_bang_rule overwrites the command and
				 * output list, so save the old pointers to
				 * be restored after do_rule().
				 */
				old_command = r->command;
				old_command_len = r->command_len;
				if(parse_bang_rule(tf, r, &tmp_nl, ext, ext ? strlen(ext) : 0) < 0)
					return -1;
			}
			struct path_list_head tmp_oo_inputs;
			TAILQ_INIT(&tmp_oo_inputs);
			if(copy_path_list(tf, &tmp_oo_inputs, &r->order_only_input_paths) < 0)
				return -1;
			if(path_list_to_nl(tf, &tmp_oo_inputs, &r->order_only_inputs, &tmp_nl, 0) < 0)
				return -1;
			free_path_list(&tmp_oo_inputs);

			if(do_rule(tf, r, &tmp_nl, ext, extlen, output_nl) < 0)
				return -1;

			if(is_bang) {
				r->command = old_command;
				r->command_len = old_command_len;
				/* If our outputs were empty, we got a copy of
				 * the bang rule's, so free our copy.
				 */
				if(outputs_empty)
					free_path_list(&r->outputs);
				free_path_list(&r->bang_extra_outputs);
				delete_name_list(&r->bang_oo_inputs);
			}

			delete_name_list(&r->order_only_inputs);
			delete_name_list_entry(&r->inputs, nle);
		}
	} else {
		/* Only parse non-foreach rules if the namelist has some
		 * entries, or if there is no input listed. We don't want to
		 * generate a command if there is an input pattern but no
		 * entries match (for example, *.o inputs to ld %f with no
		 * object files). However, if you have no input but just a
		 * command (eg: you want to run a shell script), then we still
		 * want to do the rule for that case.
		 *
		 * Also note that we check that the original user string is
		 * empty (r->empty_input), not the eval'd string. This way if
		 * the user specifies the input as $(foo) and it evaluates to
		 * empty, we won't try to execute do_rule(). But an empty user
		 * string implies that no input is required.
		 */
		if(path_list_to_nl(tf, &r->order_only_input_paths, &r->order_only_inputs, &r->inputs, 0) < 0)
			return -1;
		if((r->inputs.num_entries > 0 || r->empty_input)) {
			if(is_bang) {
				if(parse_bang_rule(tf, r, NULL, NULL, 0) < 0)
					return -1;
			}

			if(do_rule(tf, r, &r->inputs, NULL, 0, output_nl) < 0)
				return -1;

			delete_name_list(&r->inputs);
		} else {
			/* t2066 - if the inputs evaluate to empty, see if
			 * the command was a bang rule. If so, then invoke the
			 * .EMPTY rule if one was specified. If no empty rule
			 * is specified, then no command is generated.
			 */
			if(is_bang) {
				int rc;

				rc = parse_empty_bang_rule(tf, r);
				if(rc < 0)
					return -1;
				if(rc == 0) {
					if(do_rule(tf, r, &r->inputs,
						   NULL, 0, output_nl) < 0)
						return -1;
				}
			}
		}
		delete_name_list(&r->order_only_inputs);
	}

	delete_name_list(&r->bang_oo_inputs);

	return 0;
}

static int path_list_to_nl(struct tupfile *tf, struct path_list_head *plist,
			   struct name_list *nl, struct name_list *input_nl,
			   int is_variant_copy)
{
	if(eval_path_list(tf, plist, input_nl, EXPAND_NODES_SRC) < 0)
		return -1;
	if(parse_dependent_tupfiles(plist, tf) < 0)
		return -1;
	if(get_name_list(tf, plist, nl, is_variant_copy) < 0)
		return -1;
	return 0;
}

struct path_list *new_pl(struct tupfile *tf, const char *s, int len, struct bin_head *bl, int orderid)
{
	struct path_list *pl;
	char *p;

	if(len == -1)
		len = strlen(s);

	pl = malloc(sizeof(*pl) + strlen(s) + 1);
	if(!pl) {
		parser_error(tf, "malloc");
		return NULL;
	}
	pl->dir = NULL;
	pl->dirlen = 0;
	pl->group = 0;
	pl->dt = -1;
	pl->pel = NULL;
	pl->bin = NULL;
	pl->re = NULL;
	pl->re_match = NULL;
	pl->tent_relative = tf->func_frame != NULL;
	memcpy(pl->mem, s, len);
	pl->mem[len] = 0;
	pl->orderid = orderid;

	p = pl->mem;
	if(p[0] == '{') {
		/* Bin */
		char *endb;

		if(!bl) {
			fprintf(tf->f, "tup error: Bins are only usable in an input or output list.\n");
			return NULL;
		}

		endb = strchr(p, '}');
		if(!endb) {
			fprintf(tf->f, "tup error: Expecting end bracket for input bin.\n");
			return NULL;
		}
		*endb = 0;
		pl->bin = bin_find(p+1, bl);
		if(!pl->bin) {
			fprintf(tf->f, "tup error: Unable to find bin '%s'\n", p+1);
			return NULL;
		}
		*endb = '}';
	} else if(p[0] == '^') {
		/* Exclusion */
		int error;
		size_t erroffset;
		pl->re = pcre2_compile((PCRE2_SPTR)&pl->mem[1], PCRE2_ZERO_TERMINATED, 0, &error, &erroffset, NULL);
		pl->dt = exclusion_dt();
		if(!pl->re) {
			PCRE2_UCHAR buffer[256];
			pcre2_get_error_message(error, buffer, sizeof(buffer));
			fprintf(tf->f, "tup error: Unable to compile regular expression '%s' at offset %zi: %s\n", &pl->mem[1], erroffset, buffer);
			return NULL;
		}
		pl->re_match = pcre2_match_data_create_from_pattern(pl->re, NULL);
	} else {
		/* Path */
		if(strchr(p, '<') != NULL) {
			/* Group */
			char *endb;
			endb = strchr(p, '>');
			if(!endb) {
				fprintf(tf->f, "tup error: Expecting end angle bracket '>' character for group.\n");
				return NULL;
			}
			pl->group = 1;
		}
	}
	return pl;
}

static int next_path(struct tupfile *tf, const char *p, char *dest)
{
	int espace = 0;
	int quoted = 0;
	int backtick = 0;
	const char *s = p;

	for(; *s; s++) {
		if(espace) {
			*dest = *s;
			dest++;
			espace = 0;
			continue;
		} else if(*s == '\\') {
			espace = 1;
			continue;
		} else if(*s == '"') {
			quoted = !quoted;
			continue;
		} else if(*s == '`') {
			backtick = !backtick;
			*dest = *s;
			dest++;
			continue;
		} else if(isspace(*s) && !quoted && !backtick) {
			*dest = 0;
			return s - p;
		} else {
			*dest = *s;
			dest++;
		}
	}
	if(quoted) {
		fprintf(tf->f, "tup error: Missing endquote on string: %s\n", p);
		return -1;
	}
	if(backtick) {
		fprintf(tf->f, "tup error: Missing closing backtick in path: %s\n", p);
		return -1;
	}
	*dest = 0;
	return s - p;
}

static int get_path_list(struct tupfile *tf, const char *p, struct path_list_head *plist, struct bin_head *bl)
{
	struct path_list *pl;
	int orderid = 1;
	const char *s = p;

	while(*s) {
		char dest[PATH_MAX];
		int x;
		x = next_path(tf, s, dest);
		if(x < 0)
			return -1;

		pl = new_pl(tf, dest, -1, bl, orderid);
		if(!pl)
			return -1;
		orderid++;
		TAILQ_INSERT_TAIL(plist, pl, list);

		s += x;
		while(isspace(*s)) {
			s++;
		}
	}

	return 0;
}

static int eval_path_list(struct tupfile *tf, struct path_list_head *plist, struct name_list *input_nl, int expand_nodes)
{
	struct path_list *pl;
	struct path_list *tmp;

	TAILQ_FOREACH_SAFE(pl, plist, list, tmp) {
		char *eval_p;
		int spc_index;
		int last_entry = 0;
		char *p;
		char *tinput;

		if(input_nl) {
			tinput = tup_printf(tf, pl->mem, -1, input_nl, NULL, NULL, NULL, 0, NULL, EXPAND_PERCPERC);
			if(!tinput)
				return -1;
		} else {
			tinput = pl->mem;
		}
		eval_p = eval(tf, tinput, expand_nodes);
		if(!eval_p)
			return -1;
		if(input_nl)
			free(tinput);

		if(strcmp(eval_p, pl->mem) != 0) {
			p = eval_p;
			do {
				struct path_list *newpl;

				spc_index = strcspn(p, " \t");
				if(p[spc_index] == 0)
					last_entry = 1;
				if(spc_index == 0)
					goto skip_empty_space;

				newpl = new_pl(tf, p, spc_index, NULL, pl->orderid);
				if(!newpl)
					return -1;
				newpl->tent_relative = pl->tent_relative;
				if(path_uses_caller_path_var(tf, pl->mem))
					newpl->tent_relative = 0;

				TAILQ_INSERT_BEFORE(pl, newpl, list);

skip_empty_space:
				p += spc_index + 1;
			} while(!last_entry);
			del_pl(pl, plist);
		}
		free(eval_p);
	}
	return 0;
}

static int path_list_fill_dt_pel(struct tupfile *tf, struct path_list *pl, tupid_t dt, int create_output_dirs)
{
	struct pel_group pg;
	int sotgv = SOTGV_NO_GHOST;

	/* Bins get skipped. */
	if(pl->bin)
		return 0;

	/* Exclusions get skipped */
	if(pl->re)
		return 0;

	/* If we already filled it out, just return. */
	if(pl->dt != -1)
		return 0;

	if(get_path_elements(pl->mem, &pg) < 0)
		return -1;

	if(pg.pg_flags & PG_OUTSIDE_TUP) {
		if(create_output_dirs) {
			/* External output files produce an error */
			fprintf(tf->f, "tup error: Unable to write to a file outside of the tup hierarchy: %s\n", pl->mem);
			return -1;
		}
		/* External input files get skipped */
		del_pel_group(&pg);
		return 0;
	}

	if(pg.pg_flags & PG_HIDDEN) {
		fprintf(tf->f, "tup error: You specified a path '%s' that contains a hidden filename (since it begins with a '.' character). Tup ignores these files - please remove references to it from the Tupfile.\n", pl->mem);
		return -1;
	}

	if(create_output_dirs || pg.pg_flags & PG_GROUP)
		sotgv = SOTGV_CREATE_DIRS;
	pl->dt = find_dir_tupid_dt_pg(dt, &pg, &pl->pel, sotgv, 0);
	if(pl->dt <= 0) {
		fprintf(tf->f, "tup error: Failed to find directory ID for dir '%s' relative to '", pl->mem);
		print_tup_entry(tf->f, tup_entry_get(dt));
		fprintf(tf->f, "'\n");
		return -1;
	}
	if(!pl->pel) {
		if(strcmp(pl->mem, ".") == 0) {
			fprintf(tf->f, "tup error: Not expecting '.' path here.\n");
			return -1;
		}
		fprintf(tf->f, "tup internal error: Final pel missing for path: '%s'\n", pl->mem);
		return -1;
	}
	if(pl->mem != pl->pel->path) {
		/* File points to somewhere later in the path,
		 * so set the dir and dirlen parameters.
		 */
		pl->dir = pl->mem;
		pl->dirlen = pl->pel->path - pl->mem;
	}
	return 0;
}

static int copy_path_list(struct tupfile *tf, struct path_list_head *dest, struct path_list_head *src)
{
	struct path_list *pl;
	TAILQ_FOREACH(pl, src, list) {
		struct path_list *newpl;

		newpl = new_pl(tf, pl->mem, -1, current_bin_head(tf), pl->orderid);
		if(!newpl)
			return -1;
		newpl->tent_relative = pl->tent_relative;
		TAILQ_INSERT_TAIL(dest, newpl, list);
	}
	return 0;
}

void free_path_list(struct path_list_head *plist)
{
	struct path_list *pl, *tmp;

	TAILQ_FOREACH_SAFE(pl, plist, list, tmp) {
		del_pl(pl, plist);
	}
}

void del_pl(struct path_list *pl, struct path_list_head *head)
{
	TAILQ_REMOVE(head, pl, list);
	if(pl->re) {
		pcre2_match_data_free(pl->re_match);
		pcre2_code_free(pl->re);
	}
	free_pel(pl->pel);
	free(pl);
}

int parse_dependent_tupfiles(struct path_list_head *plist, struct tupfile *tf)
{
	struct path_list *pl;

	TAILQ_FOREACH(pl, plist, list) {
		tupid_t base = pl->tent_relative ? tf->curtent->tnode.tupid : tf->tent->tnode.tupid;

		if(path_list_fill_dt_pel(tf, pl, base, 0) < 0)
			return -1;
		/* Only care about non-bins, non-groups, non-exclusions,
		 * non-external files, and directories that are not our own.
		 */
		if(!pl->bin && !pl->group && !pl->re && pl->dt != -1 && pl->dt != base) {
			struct node *n;
			struct tup_entry *dtent;
			struct variant *variant;

			if(tup_entry_add(pl->dt, &dtent) < 0)
				return -1;
			variant = tup_entry_variant(dtent);
			if(variant != tf->variant && !variant->root_variant) {
				fprintf(tf->f, "tup error: Unable to use files from another variant (%s) in this variant (%s)\n", variant->variant_dir, tf->variant->variant_dir);
				return -1;
			}
			if(dtent->type == TUP_NODE_GENERATED_DIR) {
				if(tupid_tree_search(&tf->directory_root, pl->dt) == NULL) {
					fprintf(tf->f, "tup error: Unable to use inputs from a generated directory (%lli) that isn't written to by this Tupfile.\n", pl->dt);
					tup_db_print(tf->f, pl->dt);
					return -1;
				}
			}
			n = find_node(tf->g, pl->dt);
			/* We have to double check n->tent->type here in case a
			 * directory was deleted and then re-created as
			 * something else (t6073).
			 */
			if(n != NULL && !n->already_used && n->tent->type == TUP_NODE_DIR) {
				int rc;
				struct timespan ts;
				n->already_used = 1;
				rc = parse(n, tf->g, &ts, tf->refactoring, tf->use_server, tf->full_deps);
				if(rc < 0) {
					if(rc == CIRCULAR_DEPENDENCY_ERROR) {
						fprintf(tf->f, "tup error: Unable to parse dependent Tupfile due to circular directory-level dependencies: ");
						tf->circular_dep_error = 1;
					} else {
						fprintf(tf->f, "tup error: Unable to parse dependent Tupfile: ");
					}
					print_tup_entry(tf->f, n->tent);
					fprintf(tf->f, "\n");
					return -1;
				}
				/* Ignore any time the dependent Tupfile was parsing, so that
				 * we don't account for it twice.
				 */
				timespan_add_delta(&tf->ts, &ts);
			}
			if(tent_tree_add_dup(&tf->input_root, dtent) < 0)
				return -1;
		}
	}
	return 0;
}

int get_name_list(struct tupfile *tf, struct path_list_head *plist,
		  struct name_list *nl, int is_variant_copy)
{
	struct path_list *pl;

	TAILQ_FOREACH(pl, plist, list) {
		if(pl->bin) {
			if(nl_add_bin(pl->bin, nl, pl->orderid) < 0)
				return -1;
		} else if(pl->re) {
			if(nl_rm_exclusion(tf, pl, nl) < 0)
				return -1;
		} else if(pl->dt == -1) {
			if(nl_add_external_path(pl, nl, pl->orderid) < 0)
				return -1;
		} else {
			if(nl_add_path(tf, pl, nl, pl->orderid, is_variant_copy) < 0)
				return -1;
		}
	}
	return 0;
}

static int char_find(const char *s, int len, const char *list)
{
	int x;
	for(x=0; x<len; x++) {
		const char *p;
		for(p=list; *p; p++) {
			if(s[x] == *p)
				return 1;
		}
	}
	return 0;
}

static int nl_add_path(struct tupfile *tf, struct path_list *pl,
		       struct name_list *nl, int orderid, int is_variant_copy)
{
	struct build_name_list_args args;
	struct tup_entry *dtent;
	struct tup_entry *srctent = NULL;
	int checked_dtent = 0;
	int checked_srctent = 0;

	args.nl = nl;
	/* Save the original string with globs to pass to the function that
	 * determines the length, extension length, and basename length in order to
	 * handle globs.
	 */
	args.globstr = pl->pel->path;
	args.globstrlen = pl->pel->len;
	args.tf = tf;
	args.orderid = orderid;
	if(tup_entry_add(pl->dt, &dtent) < 0)
		return -1;
	if(variant_get_srctent(tf->variant, dtent, &srctent) < 0)
		return -1;
	if(char_find(pl->pel->path, pl->pel->len, "*?[") == 0) {
		struct tup_entry *tent = NULL;

		/* Only look for an input in the variant directory if it's not
		 * a !tup_preserve command (!is_variant_copy), or if this is
		 * not a variant build (tf->variant->root_variant). In the
		 * latter case, dtent is a tent in the srcdir, of course.
		 */
		if(!is_variant_copy || tf->variant->root_variant) {
			if(tup_db_select_tent_part(dtent, pl->pel->path, pl->pel->len, &tent) < 0) {
				return -1;
			}
			checked_dtent = 1;
		}
		/* If we didn't find an existing group, create one. We don't
		 * look for these in the srcdir since they are unique to each
		 * variant.
		 */
		if(!tent && pl->pel->path[0] == '<') {
			tent = tup_db_create_node_part(dtent, pl->pel->path, pl->pel->len, TUP_NODE_GROUP, -1, NULL);
			if(!tent) {
				fprintf(tf->f, "tup error: Unable to create node for group: '%.*s'\n", pl->pel->len, pl->pel->path);
				return -1;
			}
		}

		/* If we still haven't found the tent or it's a ghost in the
		 * variantdir, look for it in the srcdir for variant builds.
		 */
		if(!tent || tent->type == TUP_NODE_GHOST) {
			if(srctent) {
				if(tup_db_select_tent_part(srctent, pl->pel->path, pl->pel->len, &tent) < 0)
					return -1;
				checked_srctent = 1;
			}
		}

		if(!tent) {
			if(tf->full_deps) {
				struct stat buf;
				struct timespec mtime = INVALID_MTIME;

				if(lstat(pl->mem, &buf) == 0) {
					mtime = MTIME(buf);
				}
				if(tup_db_node_insert_tent(dtent, pl->pel->path, pl->pel->len, TUP_NODE_GHOST, mtime, -1, &tent) < 0) {
					fprintf(tf->f, "tup error: Node '%.*s' doesn't exist in directory %lli, and no luck creating a ghost node there.\n", pl->pel->len, pl->pel->path, pl->dt);
					return -1;
				}
			} else {
				fprintf(tf->f, "tup error: Explicitly named file '%.*s' not found in subdir '", pl->pel->len, pl->pel->path);
				if(checked_dtent) {
					print_tup_entry(tf->f, dtent);
					if(checked_srctent) {
						fprintf(tf->f, " or ");
					}
				}
				if(checked_srctent) {
					print_tup_entry(tf->f, srctent);
				}
				fprintf(tf->f, "'\n");
				return -1;
			}
		}
		if(tent->type == TUP_NODE_GHOST && tent->mtime.tv_sec == -1) {
			fprintf(tf->f, "tup error: Explicitly named file '%.*s' is a ghost file, so it can't be used as an input.\n", pl->pel->len, pl->pel->path);
			return -1;
		}
		if(tent_tree_search(&tf->g->gen_delete_root, tent) != NULL) {
			int valid_input = 0;

			/* If the file now exists in the srctree (ie: we
			 * deleted the rule to create a generated file and
			 * created a regular file in the srctree), then we are
			 * good (t8072).
			 */
			if(srctent) {
				struct tup_entry *tmp;
				if(tup_db_select_tent_part(srctent, pl->pel->path, pl->pel->len, &tmp) < 0)
					return -1;
				if(tmp && tmp->type != TUP_NODE_GHOST) {
					valid_input = 1;
					tent = tmp;
				}
			}

			if(!valid_input) {
				fprintf(tf->f, "tup error: Explicitly named file '%.*s' in subdir '", pl->pel->len, pl->pel->path);
				print_tupid(tf->f, pl->dt);
				fprintf(tf->f, "' is scheduled to be deleted (possibly the command that created it has been removed).\n");
				return -1;
			}
		}
		args.wildcard = 0;
		if(build_name_list_cb(&args, tent) < 0)
			return -1;
	} else {
		if(dtent->type == TUP_NODE_GHOST) {
			fprintf(tf->f, "tup error: Unable to generate wildcard for directory '%s' since it is a ghost.\n", pl->mem);
			return -1;
		}

		args.wildcard = 1;
		if(tup_db_select_node_dir_glob(build_name_list_cb, &args, dtent, pl->pel->path, pl->pel->len, &tf->g->gen_delete_root, 0) < 0)
			return -1;
		if(srctent) {
			if(tup_db_select_node_dir_glob(build_name_list_cb, &args, srctent, pl->pel->path, pl->pel->len, &tf->g->gen_delete_root, 0) < 0)
				return -1;
		}
	}
	return 0;
}

static int nl_add_external_path(struct path_list *pl, struct name_list *nl, int orderid)
{
	struct name_list_entry *nle;
	int extlesslen;

	nle = malloc(sizeof *nle);
	if(!nle) {
		perror("malloc");
		return -1;
	}

	nle->path = strdup(pl->mem);
	if(!nle->path) {
		perror("strdup");
		free(nle);
		return -1;
	}

	nle->len = strlen(nle->path);
	extlesslen = nle->len - 1;
	while(extlesslen > 0 && nle->path[extlesslen] != '.')
		extlesslen--;
	if(extlesslen == 0)
		extlesslen = nle->len;
	nle->extlesslen = extlesslen;
	nle->tent = NULL;
	set_nle_base(nle);
	nle->orderid = orderid;

	add_name_list_entry(nl, nle);
	return 0;
}

static int nl_add_bin(struct bin *b, struct name_list *nl, int orderid)
{
	struct bin_entry *be;
	struct name_list_entry *nle;
	int extlesslen;

	TAILQ_FOREACH(be, &b->entries, list) {
		extlesslen = be->len - 1;
		while(extlesslen > 0 && be->path[extlesslen] != '.')
			extlesslen--;
		if(extlesslen == 0)
			extlesslen = be->len;

		nle = malloc(sizeof *nle);
		if(!nle) {
			perror("malloc");
			return -1;
		}

		nle->path = malloc(be->len + 1);
		if(!nle->path) {
			free(nle);
			return -1;
		}
		memcpy(nle->path, be->path, be->len+1);

		nle->len = be->len;
		nle->extlesslen = extlesslen;
		nle->tent = be->tent;
		set_nle_base(nle);
		nle->orderid = orderid;

		add_name_list_entry(nl, nle);
	}
	return 0;
}

static int nl_rm_exclusion(struct tupfile *tf, struct path_list *pl, struct name_list *nl)
{
	struct name_list_entry *nle;
	struct name_list_entry *tmp;

	TAILQ_FOREACH_SAFE(nle, &nl->entries, list, tmp) {
		int rc;

		rc = pcre2_match(pl->re, (PCRE2_SPTR)nle->path, nle->len, 0, 0, pl->re_match, NULL);
		if(rc >= 0) {
			delete_name_list_entry(nl, nle);
		} else if(rc != PCRE2_ERROR_NOMATCH) {
			fprintf(tf->f, "tup error: Regex failed to execute: %s\n", &pl->mem[1]);
			return -1;
		}
	}
	return 0;
}

static int build_name_list_cb(void *arg, struct tup_entry *tent)
{
	struct build_name_list_args *args = arg;
	int extlesslen;
	int len;
	struct name_list_entry *nle;
	struct tupfile *tf = args->tf;
	struct estring e;

	/* If the file is generated from another directory, we can't use it in
	 * a wildcard.
	 * 1) srcid == -1 is for normal files
	 * 2) tent->srcid == tf->tent->tnode.tupid is for files generated by our Tupfile,
	 *    even if they are in another directory
	 * 3) tent->srcid == tent->dt is for files generated by the directory's
	 *    own Tupfile, which are still wildcard-able.
	 */
	if(tent->srcid != -1 && tent->srcid != tf->tent->tnode.tupid && tent->srcid != tent->dt) {
		struct tup_entry *srctent;
		if(args->wildcard)
			return 0;
		if(tup_entry_add(tent->srcid, &srctent) < 0)
			return -1;
		fprintf(tf->f, "tup error: Explicitly named file '");
		print_tup_entry(tf->f, tent);
		fprintf(tf->f, "' can't be listed as an input because it was generated from external directory '");
		print_tup_entry(tf->f, srctent);
		fprintf(tf->f, "' - try placing the file in a group instead and using the group as an input.\n");
		return -1;
	}

	if(estring_init(&e) < 0)
		return -1;
	if(get_relative_dir(NULL, &e, tf->srctent->tnode.tupid, tent->tnode.tupid) < 0)
		return -1;

	nle = malloc(sizeof *nle);
	if(!nle) {
		perror("malloc");
		return -1;
	}

	extlesslen = e.len;
	len = tent->name.len;
	while(len > 0 && tent->name.s[len] != '.') {
		len--;
		extlesslen--;
	}
	if(len == 0)
		extlesslen = e.len;

	nle->path = e.s;
	nle->len = e.len;
	nle->extlesslen = extlesslen;
	nle->tent = tent;
	set_nle_base(nle);
	nle->orderid = args->orderid;

	/* Do the glob parsing for %g */
	nle->globcnt = glob_parse(args->globstr, args->globstrlen, tent->name.s, nle->glob);

	add_name_list_entry(args->nl, nle);
	return 0;
}

/* Compares a globful string and the subsequent expansion to determine which
 * portions of the string are results of the glob expansion.
 *
 * This function is not guaranteed to give the same results as what sqlite3
 * parsed from the Tupfile. For instance, given the glob str:
 *     a*b*c
 * and the match:
 *     axbybzc
 * the resulting glob sections could be "x","ybz" OR "xby","z".
 * This function will allways try to match the shortest string. In this example
 * it would return the first option.
 *
 * char *base     : pointer to the globful string
 * int baselen    : length of the globful string
 * char *expanded : pointer to the matched string
 * int *globidx   : pointer to an array of index storage
 *
 * Returns: number of globs matched, or -1 on error.
 */
static int glob_parse(const char *pattern, int patlen, char *match, int *globidx)
{
	int p_it = 0;
	int m_it = 0;
	int p2_it;
	int i;
	int glob_cnt = 0;

	/* Two outputs differed, must be a wildcard */
	while(p_it < patlen) {
		while(pattern[p_it] == match[m_it] && p_it < patlen && match[m_it] != '\0') {
			/* Iterate through while the strings are the same */
			p_it++;
			m_it++;
		}

		if (p_it == patlen && match[m_it] == '\0') {
			break;
		}

		/* Found a glob */
		globidx[glob_cnt*2] = m_it;

		/* Handle all of the cases of glob characters */
		if (pattern[p_it] == '[') {
			/* User specified a range of characters. One MUST be matched.
			 * Move p_it to the character after the glob. This will be where
			 * we start looking for the next glob
			 * Need to find closing ']'
			 * Skip first character because it must be there.
			 */
			for (i=2; i<patlen-p_it; i++) {
				if (pattern[p_it+i] == ']') {
					p_it += i + 1;
					break;
				}
			}

			/* Must match one character */
			globidx[glob_cnt*2+1] = 1;
			m_it++;

		} else if (pattern[p_it] == '?') {
			/* Must match one character */
			globidx[glob_cnt*2+1] = 1;
			p_it++;
			m_it++;

		} else {
			int more_wildcards = 0;
			/* Must have found an * */
			p_it++;

			/* Skip any subsequent *. They don't mean anything. */
			while (p_it < patlen && pattern[p_it] == '*') {
				glob_cnt++;
				globidx[glob_cnt*2] = m_it;
				p_it++;
			}

			/* Need to check if there are any glob characters after the *.
			 * Parsing this case is much trickier and not supported yet.
			 */
			for (i=p_it+1; i<patlen; i++) {
				if (pattern[i] == '?' || pattern[i] == '[') {
					more_wildcards = 1;
					break;
				}
			}
			if (more_wildcards) {
				globidx[glob_cnt*2 + 1] = 0;
				glob_cnt++;
				break;

			} else {
				int non_wildcard_len;

				/* Scan for the next glob
				 * p2_it will be the idx of the next glob char
				 */
				p2_it = p_it;
				while (p2_it < patlen) {
					if (pattern[p2_it] == '*' || pattern[p2_it] == '?' || pattern[p2_it] == '[') {
						break;
					}
					p2_it++;
				}

				if (p_it == p2_it) {
					/* Asterisk at the end of the glob string
					 * Match the rest of the matched string
					 */
					globidx[glob_cnt*2 + 1] = strlen(match) - m_it;
					break;
				}

				non_wildcard_len = p2_it - p_it;

				/* Need to start at the end of the expanded string and scan
				 * to the left. This is because:
				 *     pattern:  *.txt
				 *     filename: a.txt.txt
				 * The match should be 'a.txt', not 'a'
				 */
				for (i=strlen(match)-non_wildcard_len; i>=m_it; i--) {
					int c;
					c = strncmp(pattern + p_it, match + i, non_wildcard_len);
					if (c == 0) {
						globidx[glob_cnt*2 + 1] = i - m_it;
						m_it = i;
						break;
					}
				}
			}
		}

		glob_cnt++;
		break;
	}

	return glob_cnt;
}

static int find_existing_command(const struct name_list *onl, struct tup_entry **cmd)
{
	struct name_list_entry *onle;
	TAILQ_FOREACH(onle, &onl->entries, list) {
		int rc;

		rc = tup_db_get_incoming_link(onle->tent, cmd);
		if(rc < 0)
			return -1;
		if(*cmd) {
			return 0;
		}
	}
	*cmd = NULL;
	return 0;
}

static int add_input(struct tupfile *tf, struct tent_entries *input_root,
		     struct tup_entry *tent, int force_normal_file)
{
	if(tent->type == TUP_NODE_GENERATED) {
		struct tent_entries extra_group_root = TENT_ENTRIES_INITIALIZER;
		struct tent_tree *tt;
		struct tup_entry *cmd;

		if(tent_tree_add_dup(input_root, tent) < 0)
			return -1;
		if(tup_db_get_incoming_link(tent, &cmd) < 0)
			return -1;
		if(!cmd) {
			fprintf(tf->f, "tup error: Unable to find command id for output file: ");
			print_tup_entry(tf->f, tent);
			fprintf(tf->f, "\n");
			return -1;
		}
		if(tup_db_get_inputs(cmd->tnode.tupid, &extra_group_root, NULL, NULL) < 0)
			return -1;
		RB_FOREACH(tt, tent_entries, &extra_group_root) {
			if(tt->tent->type == TUP_NODE_GROUP) {
				if(tent_tree_add_dup(input_root, tt->tent) < 0)
					return -1;
			}
		}
		free_tent_tree(&extra_group_root);
	} else if(tent->type == TUP_NODE_VAR || tent->type == TUP_NODE_GROUP) {
		if(tent_tree_add_dup(input_root, tent) < 0)
			return -1;
	} else if(tent->type == TUP_NODE_FILE) {
		if(force_normal_file) {
			if(tent_tree_add_dup(input_root, tent) < 0)
				return -1;
		}
	}
	return 0;
}

static int validate_output(struct tupfile *tf, struct tup_entry *dtent, const char *name,
			   const char *fullname, struct tent_entries *del_root)
{
	struct tup_entry *tent;

	if(tup_db_select_tent(dtent, name, &tent) < 0)
		return -1;
	if(tent) {
		if(tent->type == TUP_NODE_GHOST || tent->type == TUP_NODE_GENERATED) {
			int available = 0;
			int rc;
			struct tup_entry *incoming;

			rc = tup_db_get_incoming_link(tent, &incoming);
			if(rc < 0)
				return -1;
			if(incoming) {
				if(tent_tree_search(del_root, incoming) != NULL)
					available = 1;
				if(!available) {
					struct node *n;
					n = find_node(tf->g, tent->srcid);
					if(n != NULL && !n->parsing) {
						available = 1;
					}
				}
				if(!available) {
					fprintf(tf->f, "tup error: Unable to create output file '%s' because it is already owned by command %lli.\n", fullname, incoming->tnode.tupid);
					tup_db_print(tf->f, incoming->tnode.tupid);
					return -1;
				}
			}
		} else {
			fprintf(tf->f, "tup error: Attempting to insert '");
			if(dtent != tf->tent) {
				get_relative_dir(tf->f, NULL, tf->tent->tnode.tupid, dtent->tnode.tupid);
				fprintf(tf->f, "/");
			}
			fprintf(tf->f, "%s' as a generated node when it already exists as a different type (%s). You can do one of two things to fix this:\n  1) If this file is really supposed to be created from the command, delete the file from the filesystem and try again.\n  2) Change your rule in the Tupfile so you aren't trying to overwrite the file.\n", name, tup_db_type(tent->type));
			return -1;
		}
	}
	return 0;
}

static int do_rule_outputs(struct tupfile *tf, struct path_list_head *oplist, struct name_list *nl,
			   struct name_list *use_onl, struct name_list *onl, struct tup_entry **group,
			   int *command_modified, struct tent_entries *output_root,
			   struct tent_entries *exclusion_root,
			   const char *ext, int extlen, int is_variant_copy,
			   int transient_outputs, struct estring *variant_prefix)
{
	struct path_list *pl;
	struct path_list_head tmplist;
	struct path_list_head tmplist2;
	char flags[1];
	int flagslen;
	int rc = 0;

	TAILQ_INIT(&tmplist);
	TAILQ_INIT(&tmplist2);

	if(transient_outputs) {
		flags[0] = 't';
		flagslen = 1;
	} else {
		flags[0] = 0;
		flagslen = 0;
	}

	/* first expand any %-flags before eval so things like $(flags_%f) work */
	TAILQ_FOREACH(pl, oplist, list) {
		struct path_list *newpl;
		char *toutput;

		toutput = tup_printf(tf, pl->mem, -1, nl, use_onl, NULL, ext, extlen, NULL, NOEXPAND_PERCPERC);
		if(!toutput)
			return -1;
		newpl = new_pl(tf, toutput, -1, NULL, pl->orderid);
		if(!newpl)
			return -1;
		TAILQ_INSERT_TAIL(&tmplist, newpl, list);
		free(toutput);
	}

	/* Then eval the list so all $-variables are expanded */
	if(eval_path_list(tf, &tmplist, NULL, EXPAND_NODES_SRC) < 0)
		return -1;

	/* Use tup_printf again in case $-variables reference %-flags.
	 * tup_printf allows %O if we have a name_list (use_onl) and are not a
	 * command.
	 */
	TAILQ_FOREACH(pl, &tmplist, list) {
		struct path_list *newpl;
		char *toutput;

		toutput = tup_printf(tf, pl->mem, -1, nl, use_onl, NULL, ext, extlen, NULL, EXPAND_PERCPERC);
		if(!toutput)
			return -1;
		newpl = new_pl(tf, toutput, -1, NULL, pl->orderid);
		if(!newpl)
			return -1;
		TAILQ_INSERT_TAIL(&tmplist2, newpl, list);
		free(toutput);
	}
	free_path_list(&tmplist);

	TAILQ_FOREACH(pl, &tmplist2, list) {
		struct tup_entry *dest_tent;
		struct tup_entry *tmp_tent;
		struct name_list_entry *onle;

		if(path_list_fill_dt_pel(tf, pl, tf->tent->tnode.tupid, 1) < 0)
			return -1;
		if(build_ctx_strict_check(tf, pl) < 0)
			return -1;

		if(tup_entry_add(pl->dt, &dest_tent) < 0)
			return -1;

		if(pl->group) {
			if(*group) {
				fprintf(tf->f, "tup error: Multiple output groups detected: '");
				print_tup_entry(tf->f, *group);
				fprintf(tf->f, "' and '%s/%.*s'\n", pl->mem, pl->pel->len, pl->pel->path);
				return -1;
			}

			*group = tup_db_create_node_part(dest_tent, pl->pel->path, pl->pel->len, TUP_NODE_GROUP, -1, NULL);
			if(!*group)
				return -1;
			continue;
		}
		if(pl->re) {
			struct tup_entry *tent;

			tent = tup_db_create_node(dest_tent, &pl->mem[1], TUP_NODE_GHOST);
			if(!tent) {
				fprintf(tf->f, "tup error: Unable to create exclusion output node for: %s\n", pl->mem);
				return -1;
			}
			if(tent_tree_add(exclusion_root, tent) < 0) {
				fprintf(tf->f, "tup error: The exclusion '%s' is listed multiple times in a command.\n", pl->mem);
				rc = -1;
				continue;
			}
			continue;
		}


		onle = malloc(sizeof *onle);
		if(!onle) {
			parser_error(tf, "malloc");
			return -1;
		}
		onle->orderid = pl->orderid;

		if(pl->dir) {
			onle->path = malloc(pl->dirlen + pl->pel->len + 1);
			if(!onle->path) {
				perror("malloc");
				return -1;
			}
			strncpy(onle->path, pl->dir, pl->dirlen);
			strncpy(onle->path + pl->dirlen, pl->pel->path, pl->pel->len);
			onle->path[pl->dirlen + pl->pel->len] = 0;
		} else {
			onle->path = malloc(pl->pel->len + 1);
			if(!onle->path) {
				perror("malloc");
				return -1;
			}
			strncpy(onle->path, pl->pel->path, pl->pel->len);
			onle->path[pl->pel->len] = 0;
		}
		if(!tf->variant->root_variant) {
			char *withvariant;
			struct tup_entry *tent;
			struct tup_entry *srctent;
			if(variant_get_srctent(tf->variant, dest_tent, &srctent) < 0)
				return -1;
			if(srctent && !is_variant_copy) {
				if(tup_db_select_tent(srctent, onle->path, &tent) < 0)
					return -1;
				if(tent && tent->type != TUP_NODE_GHOST) {
					fprintf(tf->f, "tup error: Attempting to insert '%s' as a generated node when it already exists as a different type (%s) in the source directory. You can do one of two things to fix this:\n  1) If this file is really supposed to be created from the command, delete the file from the filesystem and try again.\n  2) Change your rule in the Tupfile so you aren't trying to overwrite the file.\n", onle->path, tup_db_type(tent->type));
					return -1;
				}
			}
			withvariant = malloc(variant_prefix->len + 1 + strlen(onle->path) + 1);
			if(!withvariant) {
				perror("malloc");
				return -1;
			}
			memcpy(withvariant, variant_prefix->s, variant_prefix->len);
			withvariant[variant_prefix->len] = '/';
			strcpy(withvariant+variant_prefix->len+1, onle->path);
			free(onle->path);
			onle->path = withvariant;
		}
		if(name_cmp(onle->path, "Tupfile") == 0 ||
		   name_cmp(onle->path, "Tuprules.tup") == 0 ||
		   name_cmp(onle->path, "Tupfile.lua") == 0 ||
		   name_cmp(onle->path, "Tuprules.lua") == 0 ||
		   name_cmp(onle->path, TUP_CONFIG) == 0) {
			fprintf(tf->f, "tup error: Attempted to generate a file called '%s', which is reserved by tup. Your build configuration must be comprised of files you write yourself.\n", onle->path);
			free(onle->path);
			free(onle);
			rc = -1;
			continue;
		}
		onle->len = strlen(onle->path);
		onle->extlesslen = onle->len - 1;
		while(onle->extlesslen > 0 && onle->path[onle->extlesslen] != '.')
			onle->extlesslen--;

		/* Go up until we find a non-generated dir, so we can try to
		 * gitignore there.
		 */
		tmp_tent = dest_tent;
		while(tmp_tent->type == TUP_NODE_GENERATED_DIR) {
			tmp_tent = tmp_tent->parent;
		}
		if(tent_tree_add_dup(&tf->g->parse_gitignore_root, tmp_tent) < 0)
			return -1;

		set_nle_base(onle);
		if(validate_output(tf, dest_tent, onle->base, onle->path, &tf->g->cmd_delete_root) < 0) {
			rc = -1;
			continue;
		}
		if(tupid_tree_add_dup(&tf->directory_root, pl->dt) < 0)
			return -1;
		onle->tent = tup_db_create_node_part_display(dest_tent, onle->base, -1,
							     NULL, 0,
							     flags, flagslen,
							     TUP_NODE_GENERATED, tf->tent->tnode.tupid,
							     command_modified);
		if(!onle->tent) {
			free(onle->path);
			free(onle);
			return -1;
		}
		if(tent_tree_add(output_root, onle->tent) < 0) {
			fprintf(tf->f, "tup error: The output file '%s' is listed multiple times in a command.\n", onle->path);
			rc = -1;
			continue;
		}

		add_name_list_entry(onl, onle);
	}
	free_path_list(&tmplist2);
	return rc;
}

struct command_split {
	const char *flags;
	int flagslen;
	const char *display;
	int displaylen;
	const char *cmd;
};

static int split_command_string(struct tupfile *tf, const char *cmd, struct command_split *cs)
{
	cs->flags = "";
	cs->flagslen = 0;
	cs->display = NULL;
	cs->displaylen = 0;
	cs->cmd = NULL;
	const char *s = cmd;
	if(s[0] == '^') {
		s++;
		if(*s != ' ') {
			cs->flags = s;
			do {
				if(!*s) {
					fprintf(tf->f, "tup error: Missing ending '^' flag in command string: %s\n", cmd);
					return -1;
				}
				if(*s == ' ')
					break;
				if(s[0] == '^')
					break;
				s++;
			} while(1);
			cs->flagslen = s - cs->flags;
		}
		if(*s == '^') {
			/* Only flags - no display */
			s++;
			while(isspace(*s)) s++;
			cs->display = NULL;
			cs->displaylen = 0;
		} else {
			while(isspace(*s)) s++;
			cs->display = s;
			do {
				if(!*s) {
					fprintf(tf->f, "tup error: Missing ending '^' flag in command string: %s\n", cmd);
					return -1;
				}
				if(s[0] == '^')
					break;
				s++;
			} while(1);
			cs->displaylen = s - cs->display;
			s++;
		}
	}
	while(isspace(*s)) s++;
	cs->cmd = s;
	return 0;
}

static int do_rule(struct tupfile *tf, struct rule *r, struct name_list *nl,
		   const char *ext, int extlen, struct name_list *output_nl)
{
	struct name_list onl;
	struct name_list extra_onl;
	struct name_list_entry *nle, *onle;
	char *tcmd;
	char *cmd;
	char *real_display;
	int real_displaylen;
	struct tent_tree *tt;
	struct tent_tree *ttree;
	struct tup_entry *cmdtent = NULL;
	struct tent_entries input_root = TENT_ENTRIES_INITIALIZER;
	struct tent_entries output_root = TENT_ENTRIES_INITIALIZER;
	struct tent_entries exclusion_root = TENT_ENTRIES_INITIALIZER;
	struct tup_entry *tmptent = NULL;
	struct tup_entry *group = NULL;
	struct tup_entry *old_group = NULL;
	struct estring variant_prefix;
	int command_modified = 0;
	int is_variant_copy = 0;
	int compare_display_flags = 0;
	int transient_outputs = 0;
	struct command_split cs;

	/* t3017 - empty rules are just pass-through to get the input into the
	 * bin.
	 */
	if(r->command_len == 0) {
		if(r->bin) {
			TAILQ_FOREACH(nle, &nl->entries, list) {
				if(bin_add_entry(r->bin, nle->path, nle->len, nle->tent) < 0)
					return -1;
			}
		}
		return 0;
	}

	if(split_command_string(tf, r->command, &cs) < 0)
		return -1;
	if(memchr(cs.flags, 't', cs.flagslen) != NULL)
		transient_outputs = 1;
	if(memchr(cs.flags, 'o', cs.flagslen) != NULL && transient_outputs) {
		fprintf(tf->f, "tup error: Unable to use both 'o' and 't' flags at the same time. Outputs cannot be compared with 'o' if the files are deleted after they are used with 't'.\n");
		return -1;
	}

	if(strcmp(cs.cmd, TUP_PRESERVE_CMD) == 0)
		is_variant_copy = 1;

	init_name_list(&onl);
	init_name_list(&extra_onl);

	estring_init(&variant_prefix);
	if(tf->srctent != tf->tent) {
		if(get_relative_dir(NULL, &variant_prefix, tf->srctent->tnode.tupid, tf->tent->tnode.tupid) < 0)
			return -1;
	}
	if(do_rule_outputs(tf, &r->outputs, nl, NULL, &onl, &group, &command_modified, &output_root, &exclusion_root, ext, extlen, is_variant_copy, transient_outputs, &variant_prefix) < 0)
		return -1;
	if(r->bin) {
		TAILQ_FOREACH(onle, &onl.entries, list) {
			if(bin_add_entry(r->bin, onle->path, onle->len, onle->tent) < 0)
				return -1;
		}
	}
	if(do_rule_outputs(tf, &r->extra_outputs, nl, &onl, &extra_onl, &group, &command_modified, &output_root, &exclusion_root, ext, extlen, is_variant_copy, transient_outputs, &variant_prefix) < 0)
		return -1;
	if(do_rule_outputs(tf, &r->bang_extra_outputs, nl, &onl, &extra_onl, &group, &command_modified, &output_root, &exclusion_root, ext, extlen, is_variant_copy, transient_outputs, &variant_prefix) < 0)
		return -1;
	free(variant_prefix.s);

	if(is_variant_copy) {
		if(onl.num_entries != 1 || nl->num_entries != 1) {
			fprintf(tf->f, "tup error: !tup_preserve requires a single input file.\n");
			return -1;
		}
	}

	tcmd = tup_printf(tf, cs.cmd, -1, nl, &onl, &r->order_only_inputs, ext, extlen, r->extra_command, EXPAND_PERCPERC);
	if(!tcmd)
		return -1;
	cmd = eval(tf, tcmd, EXPAND_NODES_CMD);
	if(!cmd)
		return -1;
	free(tcmd);

	if(cs.display) {
		real_display = tup_printf(tf, cs.display, cs.displaylen, nl, &onl, &r->order_only_inputs, ext, extlen, NULL, EXPAND_PERCPERC);
		if(!real_display)
			return -1;
		real_displaylen = strlen(real_display);
	} else {
		real_display = NULL;
		real_displaylen = 0;
	}

	/* If we already have our command string in the db, then use that.
	 * Otherwise, we try to find an existing command of a different
	 * name that points to the output files we are trying to create.
	 * If neither of those cases apply, we just create a new command
	 * node. Note we require a case-sensitive comparison, since we want to
	 * re-run the command if the case of a string or filename has changed.
	 */
	if(tup_db_select_tent(tf->tent, cmd, &tmptent) < 0)
		return -1;
	if(tmptent && strcmp(tmptent->name.s, cmd) == 0) {
		cmdtent = tmptent;
		if(tmptent->type != TUP_NODE_CMD) {
			fprintf(tf->f, "tup error: Unable to create command '%s' because the node already exists in the database as type '%s'\n", cmd, tup_db_type(tmptent->type));
			return -1;
		}
		compare_display_flags = 1;
	} else {
		if(find_existing_command(&onl, &cmdtent) < 0)
			return -1;
		if(!cmdtent) {
			tupid_t cmdid;
			if(tf->refactoring) {
				fprintf(tf->f, "tup refactoring error: Attempting to create a new command: %s\n", cmd);
				return -1;
			}
			cmdid = create_command_file(tf->tent->tnode.tupid, cmd, real_display, real_displaylen, cs.flags, cs.flagslen);
			if(tup_entry_add(cmdid, &cmdtent) < 0)
				return -1;
		} else {
			if(tf->refactoring) {
				fprintf(tf->f, "tup refactoring error: Attempting to modify a command string:\n");
				fprintf(tf->f, "Old: '%s'\n", cmdtent->name.s);
				fprintf(tf->f, "New: '%s'\n", cmd);
				return -1;
			}
			if(tup_db_set_name(cmdtent->tnode.tupid, cmd, tf->tent->tnode.tupid) < 0)
				return -1;

			/* Since we changed the name, we have to run the
			 * command again.
			 */
			command_modified = 1;

			compare_display_flags = 1;
		}
	}
	if(compare_display_flags) {
		if(cmdtent->displaylen != real_displaylen || (real_displaylen > 0 && strncmp(cmdtent->display, real_display, real_displaylen) != 0)) {
			if(tup_db_set_display(cmdtent, real_display, real_displaylen) < 0)
				return -1;
		}
		if(cmdtent->flagslen != cs.flagslen || (cs.flagslen > 0 && strncmp(cmdtent->flags, cs.flags, cs.flagslen)) != 0) {
			if(tf->refactoring) {
				fprintf(tf->f, "tup refactoring error: Attempting to modify a command's flags:\n");
				fprintf(tf->f, "Old: '%.*s'\n", cmdtent->flagslen, cmdtent->flags);
				fprintf(tf->f, "New: '%.*s'\n", cs.flagslen, cs.flags);
				return -1;
			}
			if(tup_db_set_flags(cmdtent, cs.flags, cs.flagslen) < 0)
				return -1;
			command_modified = 1;
		}
	}

	free(real_display);
	free(cmd);
	if(!cmdtent)
		return -1;

	if(tupid_tree_add(&tf->cmd_root, cmdtent->tnode.tupid) < 0) {
		fprintf(tf->f, "tup error: Attempted to add duplicate command ID %lli\n", cmdtent->tnode.tupid);
		tup_db_print(tf->f, cmdtent->tnode.tupid);
		return -1;
	}
	tent_tree_remove(&tf->g->cmd_delete_root, cmdtent);
	if(tf->refactoring) {
		tent_tree_remove(&tf->refactoring_cmd_delete_root, cmdtent);
	}

	while(!TAILQ_EMPTY(&onl.entries)) {
		onle = TAILQ_FIRST(&onl.entries);

		if(tup_db_create_unique_link(cmdtent, onle->tent) < 0) {
			return -1;
		}
		tent_tree_remove(&tf->g->gen_delete_root, onle->tent);
		tent_tree_remove(&tf->g->save_root, onle->tent);
		if(output_nl) {
			move_name_list_entry(output_nl, &onl, onle);
		} else {
			delete_name_list_entry(&onl, onle);
		}
	}

	while(!TAILQ_EMPTY(&extra_onl.entries)) {
		onle = TAILQ_FIRST(&extra_onl.entries);
		if(tup_db_create_unique_link(cmdtent, onle->tent) < 0) {
			return -1;
		}
		tent_tree_remove(&tf->g->gen_delete_root, onle->tent);
		tent_tree_remove(&tf->g->save_root, onle->tent);
		if(output_nl) {
			move_name_list_entry(output_nl, &extra_onl, onle);
		} else {
			delete_name_list_entry(&extra_onl, onle);
		}
	}

	TAILQ_FOREACH(nle, &nl->entries, list) {
		if(nle->tent)
			if(add_input(tf, &input_root, nle->tent, 1) < 0)
				return -1;
	}
	TAILQ_FOREACH(nle, &r->order_only_inputs.entries, list) {
		if(nle->tent)
			if(add_input(tf, &input_root, nle->tent, 0) < 0)
				return -1;
	}
	TAILQ_FOREACH(nle, &r->bang_oo_inputs.entries, list) {
		if(nle->tent)
			if(add_input(tf, &input_root, nle->tent, 0) < 0)
				return -1;
	}
	RB_FOREACH(tt, tent_entries, &tf->env_root) {
		if(add_input(tf, &input_root, tt->tent, 0) < 0)
			return -1;
	}
	if(group) {
		/* This is a quick check for a simple circular dependency that
		 * can be done before the full check after all parsing is
		 * complete.
		 */
		if(tent_tree_search(&input_root, group) != NULL) {
			fprintf(tf->f, "tup error: Command ID %lli both reads from and writes to this group: ", cmdtent->tnode.tupid);
			print_tup_entry(tf->f, group);
			fprintf(tf->f, "\n");
			tup_db_print(tf->f, cmdtent->tnode.tupid);
			return -1;
		}
	}

	RB_FOREACH(ttree, tent_entries, &output_root) {
		if(tent_tree_search(&input_root, ttree->tent) != NULL) {
			fprintf(tf->f, "tup error: Command ID %lli lists this file as both an input and an output: ", cmdtent->tnode.tupid);
			print_tup_entry(tf->f, ttree->tent);
			fprintf(tf->f, "\n");
			return -1;
		}
	}
	if(tup_db_write_outputs(tf->f, cmdtent, &output_root, &exclusion_root, group, &old_group, tf->refactoring, command_modified) < 0)
		return -1;
	if(tup_db_write_inputs(tf->f, cmdtent->tnode.tupid, &input_root, &tf->env_root, group, old_group, tf->refactoring) < 0)
		return -1;
	free_tent_tree(&exclusion_root);
	free_tent_tree(&output_root);
	free_tent_tree(&input_root);
	return 0;
}

void init_name_list(struct name_list *nl)
{
	TAILQ_INIT(&nl->entries);
	nl->num_entries = 0;
	nl->totlen = 0;
	nl->basetotlen = 0;
	nl->extlessbasetotlen = 0;
	memset(nl->globtotlen, 0, MAX_GLOBS * sizeof(int));
	nl->globcnt = 0;
}

static void set_nle_base(struct name_list_entry *nle)
{
	nle->base = nle->path + nle->len;
	nle->baselen = 0;
	while(nle->base > nle->path) {
		nle->base--;
		if(is_path_sep(nle->base)) {
			nle->base++;
			goto out;
		}
		nle->baselen++;
	}
out:
	/* The extension-less baselen is the length of the base, minus the
	 * length of the extension we calculated before.
	 */
	nle->extlessbaselen = nle->baselen - (nle->len - nle->extlesslen);
	nle->globcnt = 0;
}

static void add_name_list_entry(struct name_list *nl,
				struct name_list_entry *nle)
{
	TAILQ_INSERT_TAIL(&nl->entries, nle, list);
	nl->num_entries++;
	nl->totlen += nle->len;
	nl->basetotlen += nle->baselen;
	nl->extlessbasetotlen += nle->extlessbaselen;
	{
		int i;
		for (i=0; i<nle->globcnt; i++) {
			nl->globtotlen[i] += nle->glob[i*2+1];
		}
	}
	nl->globcnt = nle->globcnt;
}

void delete_name_list(struct name_list *nl)
{
	struct name_list_entry *nle;
	while(!TAILQ_EMPTY(&nl->entries)) {
		nle = TAILQ_FIRST(&nl->entries);
		delete_name_list_entry(nl, nle);
	}
}

static void delete_name_list_entry(struct name_list *nl,
				   struct name_list_entry *nle)
{
	nl->num_entries--;
	nl->totlen -= nle->len;
	nl->basetotlen -= nle->baselen;
	nl->extlessbasetotlen -= nle->extlessbaselen;
	{
		int i;
		for (i=0; i<nle->globcnt; i++) {
			nl->globtotlen[i] -= nle->glob[i*2+1];
		}
	}

	TAILQ_REMOVE(&nl->entries, nle, list);
	free(nle->path);
	free(nle);
}

void move_name_list_entry(struct name_list *newnl, struct name_list *oldnl,
			  struct name_list_entry *nle)
{
	oldnl->num_entries--;
	oldnl->totlen -= nle->len;
	oldnl->basetotlen -= nle->baselen;
	oldnl->extlessbasetotlen -= nle->extlessbaselen;
	{
		int i;
		for (i=0; i<nle->globcnt; i++) {
			oldnl->globtotlen[i] -= nle->glob[i*2+1];
		}
	}

	TAILQ_REMOVE(&oldnl->entries, nle, list);

	newnl->num_entries++;
	newnl->totlen += nle->len;
	newnl->basetotlen += nle->baselen;
	newnl->extlessbasetotlen += nle->extlessbaselen;
	{
		int i;
		for (i=0; i<nle->globcnt; i++) {
			newnl->globtotlen[i] += nle->glob[i*2+1];
		}
	}
	newnl->globcnt = nle->globcnt;
	TAILQ_INSERT_TAIL(&newnl->entries, nle, list);
}

static const char *find_char(const char *s, int len, char c)
{
	int x;
	for(x=0; x<len; x++) {
		if(s[x] == c)
			return &s[x];
	}
	return NULL;
}

static char *tup_printf(struct tupfile *tf, const char *cmd, int cmd_len,
			struct name_list *nl, struct name_list *onl,
			struct name_list *ooinput_nl,
			const char *ext, int extlen,
			const char *extra_command,
			int expand_percperc)
{
	struct name_list_entry *nle;
	const char *p;
	const char *next;
	struct estring e;

	if(!nl) {
		fprintf(tf->f, "tup internal error: tup_printf called with NULL name_list\n");
		return NULL;
	}

	if(cmd_len == -1) {
		cmd_len = strlen(cmd);
	}

	if(estring_init(&e) < 0)
		return NULL;

	p = cmd;
	while((next = find_char(p, cmd+cmd_len - p, '%')) !=  NULL) {
		if(next == cmd+cmd_len-1) {
			fprintf(tf->f, "tup error: Unfinished %%-flag at the end of the string '%s'\n", cmd);
			return NULL;
		}
		estring_append(&e, p, next-p);

		next++;
		p = next + 1;

		if(*next == 'f') {
			int first = 1;
			if(nl->num_entries == 0) {
				fprintf(tf->f, "tup error: %%f used in rule pattern and no input files were specified.\n");
				return NULL;
			}
			TAILQ_FOREACH(nle, &nl->entries, list) {
				if(!first) {
					estring_append(&e, " ", 1);
				}
				estring_append(&e, nle->path, nle->len);
				first = 0;
			}
		} else if(*next == 'b') {
			int first = 1;
			if(nl->num_entries == 0) {
				fprintf(tf->f, "tup error: %%b used in rule pattern and no input files were specified.\n");
				return NULL;
			}
			TAILQ_FOREACH(nle, &nl->entries, list) {
				if(!first) {
					estring_append(&e, " ", 1);
				}
				estring_append(&e, nle->base, nle->baselen);
				first = 0;
			}
		} else if(*next == 'B') {
			int first = 1;
			if(nl->num_entries == 0) {
				fprintf(tf->f, "tup error: %%B used in rule pattern and no input files were specified.\n");
				return NULL;
			}
			TAILQ_FOREACH(nle, &nl->entries, list) {
				if(!first) {
					estring_append(&e, " ", 1);
				}
				estring_append(&e, nle->base, nle->extlessbaselen);
				first = 0;
			}
		} else if(*next == 'i') {
			int first = 1;
			if(!ooinput_nl) {
				fprintf(tf->f, "tup error: %%i is only valid in a command string.\n");
				return NULL;
			} else if(ooinput_nl->num_entries == 0) {
				fprintf(tf->f, "tup error: %%i used in rule pattern and no order-only input files were specified.\n");
				return NULL;
			}
			TAILQ_FOREACH(nle, &ooinput_nl->entries, list) {
				if(!first) {
					estring_append(&e, " ", 1);
				}
				estring_append(&e, nle->path, nle->len);
				first = 0;
			}
		} else if(*next == 'e') {
			if(!ext) {
				fprintf(tf->f, "tup error: %%e is only valid with a foreach rule for files that have extensions.\n");
				if(nl->num_entries == 1) {
					nle = TAILQ_FIRST(&nl->entries);
					fprintf(tf->f, " -- Path: '%s'\n", nle->path);
				} else {
					fprintf(tf->f, " -- This does not appear to be a foreach rule\n");
				}
				return NULL;
			}
			estring_append(&e, ext, extlen);
		} else if(*next == 'o') {
			int first = 1;
			if(!onl) {
				fprintf(tf->f, "tup error: %%o can only be used in a command string or extra outputs section.\n");
				return NULL;
			}
			if(onl->num_entries == 0) {
				fprintf(tf->f, "tup error: %%o used in rule pattern and no output files were specified.\n");
				return NULL;
			}
			TAILQ_FOREACH(nle, &onl->entries, list) {
				if(!first) {
					estring_append(&e, " ", 1);
				}
				estring_append(&e, nle->path, nle->len);
				first = 0;
			}
		} else if(*next == 'O') {
			if(!onl) {
				fprintf(tf->f, "tup error: %%O can only be used in the extra outputs section.\n");
				return NULL;
			}
			if(onl->num_entries != 1) {
				fprintf(tf->f, "tup error: %%O can only be used if there is exactly one output specified.\n");
				return NULL;
			}
			nle = TAILQ_FIRST(&onl->entries);
			estring_append(&e, nle->path, nle->extlesslen);
		} else if(*next == '\'' || *next == '"') {
			if(*p == 'f') {
				int first = 1;
				if(nl->num_entries == 0) {
					fprintf(tf->f, "tup error: %%%cf used in rule pattern and no input files were specified.\n", *next);
					return NULL;
				}
				TAILQ_FOREACH(nle, &nl->entries, list) {
					if(!first) {
						estring_append(&e, " ", 1);
					}
					estring_append(&e, next, 1);
					estring_append_escape(&e, nle->path, nle->len, *next);
					estring_append(&e, next, 1);
					first = 0;
				}
			} else if(*p == 'o') {
				int first = 1;
				if(!onl) {
					fprintf(tf->f, "tup error: %%%co can only be used in a command string or extra outputs section.\n", *next);
					return NULL;
				}
				if(onl->num_entries == 0) {
					fprintf(tf->f, "tup error: %%%co used in rule pattern and no output files were specified.\n", *next);
					return NULL;
				}
				TAILQ_FOREACH(nle, &onl->entries, list) {
					if(!first) {
						estring_append(&e, " ", 1);
					}
					estring_append(&e, next, 1);
					estring_append_escape(&e, nle->path, nle->len, *next);
					estring_append(&e, next, 1);
					first = 0;
				}
			} else {
				fprintf(tf->f, "tup error: %%%c must be followed by an 'f' for input files or an 'o' for output files.\n", *next);
				return NULL;
			}
			p++;
		} else if(*next == 'd') {
			if(tf->tent->tnode.tupid == DOT_DT) {
				/* At the top of the tup-hierarchy, we get the
				 * directory from where .tup is stored, since
				 * the top-level tup entry is just "."
				 */
				const char *last_slash;
				const char *dirstring;
				int len;

				last_slash = strrchr(get_tup_top(), path_sep());
				if(last_slash) {
					dirstring = last_slash + 1;
				} else {
					dirstring = get_tup_top();
				}
				len = strlen(dirstring);
				estring_append(&e, dirstring, len);
			} else {
				struct tup_entry *tent;
				if(tup_entry_add(tf->tent->tnode.tupid, &tent) < 0)
					return NULL;
				/* Anywhere else in the hierarchy can just use
				 * the last tup entry of the parsed directory
				 * as the %d replacement.
				 */
				estring_append(&e, tent->name.s, tent->name.len);
			}
		} else if(*next == 'g') {
			/* g: Expands to the "glob" portion of an *, ?, [] expansion.
			 *    Given the filnames: a_text.txt, b_text.txt and c_text.txt,
			 *    and the tupfiles:   : foreach *_text.txt |> foo %f |> %g_binary.bin
			 *                        : foreach ?_text.txt |> foo %f |> %g_binary.bin
			 *                        : foreach [abc]_text.txt |> foo %f |> %g_binary.bin
			 *    then outputs ->:    a_binary.bin, b_binary.bin, c_binary.bin
			 */
			if(nl->num_entries == 0) {
				fprintf(tf->f, "tup error: %%g used in rule pattern and no input files were specified.\n");
				return NULL;
			}
			if(nl->num_entries > 1) {
				fprintf(tf->f, "tup error: %%g is only valid with one file.\n");
				return NULL;
			}
			if(nl->globcnt == 0) {
				fprintf(tf->f, "tup error: %%g flag found no globs.\n");
				return NULL;
			}
			TAILQ_FOREACH(nle, &nl->entries, list) {
				estring_append(&e, nle->base + nle->glob[0], nle->glob[1]);
			}
		} else if(isdigit(*next)) {
			struct name_list *tmpnl = NULL;
			char *endp;
			int num;
			int first = 1;
			errno = 0;
			num = strtol(next, &endp, 10);
			if(errno) {
				perror("strtol");
				fprintf(tf->f, "tup error: Failed to run strtol on %%-flag with a number.\n");
				return NULL;
			}
			/* Bounds-check the %-flag number, except for node
			 * references (%Nt) which use an internal tup id.
			 */
			if((num <= 0 || num >= 99) && *endp != 't') {
				fprintf(tf->f, "tup error: Expected number from 1-99 (base 10) for %%-flag, but got %i\n", num);
				return NULL;
			}
			if (*endp == '\0') {
				fprintf(tf->f, "tup error: Unfinished %%%i-flag at the end of the string '%s'\n", num, cmd);
				return NULL;
			} else if(strchr("fBb", *endp)) {
				tmpnl = nl;
			} else if(*endp == 'o') {
				tmpnl = onl;
			} else if(*endp == 'i') {
				if(!ooinput_nl) {
					fprintf(tf->f, "tup error: %%%ii is only valid in a command string.\n", num);
					return NULL;
				}
				tmpnl = ooinput_nl;
			} else if(*endp == 't') {
				/* Skip node references here, they are
				 * resolved in eval(). Just store the %Nt
				 * string back in the output.
				 */
				estring_append(&e, "%%", 1);
				estring_append(&e, next, endp-next + 1);
			} else {
				fprintf(tf->f, "tup error: Expected 'f', 'b', 'B', 'o', or 'i' after number in %%%i-flag, but got '%c'\n", num, *endp);
				return NULL;
			}
			if(tmpnl) {
				TAILQ_FOREACH(nle, &tmpnl->entries, list) {
					if(nle->orderid == num) {
						if(!first && estring_append(&e, " ", 1) < 0)
							return NULL;
						int err;
						if(*endp == 'B') {
							err = estring_append(&e, nle->base, nle->extlessbaselen);
						} else if(*endp == 'b') {
							err = estring_append(&e, nle->base, nle->baselen);
						} else {
							err = estring_append(&e, nle->path, nle->len);
						}
						if(err < 0)
							return NULL;
						first = 0;
					} else if(nle->orderid > num) {
						break;
					}
				}
			}

			p = endp+1;
		} else if(*next == '<') {
			/* %<group> is expanded by the updater before executing
			 * a command.
			 */
			estring_append(&e, "%<", 2);
		} else if(*next == '%') {
			estring_append(&e, "%", 1);
			if(expand_percperc == NOEXPAND_PERCPERC) {
				/* If we aren't expanding %% into % yet, keep
				 * it as %% in the output.
				 */
				estring_append(&e, "%", 1);
			}
		} else {
			fprintf(tf->f, "tup error: Unknown %%-flag: '%c'\n", *next);
			return NULL;
		}
	}
	estring_append(&e, p, cmd+cmd_len - p);

	if(extra_command) {
		char *textra;

		textra = tup_printf(tf, extra_command, strlen(extra_command), nl, onl, ooinput_nl, ext, extlen, NULL, EXPAND_PERCPERC);
		if(!textra)
			return NULL;
		estring_append(&e, " ", 1);
		estring_append(&e, textra, strlen(textra));
		free(textra);
	}
	return e.s;
}

static char *expand_node_strings(struct tupfile *tf, const char *string, int expand_nodes)
{
	struct estring e;
	tupid_t base_tupid;
	const char *s;

	if(estring_init(&e) < 0)
		return NULL;
	if(expand_nodes == EXPAND_NODES_CMD) {
		base_tupid = tf->tent->tnode.tupid;
	} else {
		base_tupid = variant_tent_to_srctent(tf->curtent)->tnode.tupid;
	}
	s = string;
	while(*s) {
		if(*s == '%' && isdigit(s[1])) {
			char *endp;
			tupid_t tupid;
			errno = 0;
			tupid = strtoll(s+1, &endp, 10);
			if(!errno && *endp == 't') {
				/* Internal-only node reference */
				struct tup_entry *tent;
				if(tup_entry_add(tupid, &tent) < 0)
					return NULL;

				/* Inputs & outputs are always given as if
				 * they're in the srcdir, even if the node
				 * variable points inside the variantdir due to
				 * $(TUP_VARIANTDIR).
				 */
				if(expand_nodes == EXPAND_NODES_SRC)
					tent = variant_tent_to_srctent(tent);

				if(get_relative_dir(NULL, &e, base_tupid, tent->tnode.tupid) < 0)
					return NULL;
				s = endp + 1;
			} else {
				if(estring_append(&e, s, 1) < 0)
					return NULL;
				s++;
			}
		} else {
			if(estring_append(&e, s, 1) < 0)
				return NULL;
			s++;
		}
	}
	if(estring_append(&e, s, strlen(s)) < 0)
		return NULL;
	return e.s;
}

char *eval(struct tupfile *tf, const char *string, int expand_nodes)
{
	const char *s;
	const char *var;
	const char *syntax_msg = "oops";
	struct estring e;

	if(estring_init(&e) < 0)
		return NULL;
	s = string;
	while(*s) {
		if(*s == '\\') {
			if((s[1] == '$' || s[1] == '@') && s[2] == '(') {
				/* \$( becomes $( */
				/* \@( becomes @( */
				if(estring_append(&e, &s[1], 1) < 0)
					return NULL;
				s += 2;
			} else {
				if(estring_append(&e, s, 1) < 0)
					return NULL;
				s++;
			}
		} else if(*s == '$') {
			const char *rparen;

			if(s[1] == '(') {
				rparen = find_matching_paren(s+2);
				if(!rparen) {
					syntax_msg = "expected ending variable paren ')'";
					goto syntax_error;
				}

				var = s + 2;
				if(rparen-var == 7 &&
				   strncmp(var, "TUP_CWD", 7) == 0) {
					if(get_relative_dir(NULL, &e, tf->tent->tnode.tupid, tf->curtent->tnode.tupid) < 0) {
						fprintf(tf->f, "tup internal error: Unable to find relative directory from ID %lli -> %lli\n", tf->tent->tnode.tupid, tf->curtent->tnode.tupid);
						tup_db_print(tf->f, tf->tent->tnode.tupid);
						tup_db_print(tf->f, tf->curtent->tnode.tupid);
						return NULL;
					}
				} else if(rparen-var == 14 &&
					  strncmp(var, "TUP_VARIANTDIR", 14) == 0) {
					char value[32];
					snprintf(value, 31, "%%%llit", tf->curtent->tnode.tupid);
					value[31] = 0;
					if(estring_append(&e, value, strlen(value)) < 0)
						return NULL;
				} else if(rparen-var == 21 &&
					  strncmp(var, "TUP_VARIANT_OUTPUTDIR", 21) == 0) {
					if(get_relative_dir(NULL, &e, tf->srctent->tnode.tupid, tf->tent->tnode.tupid) < 0) {
						fprintf(tf->f, "tup internal error: Unable to find relative directory from ID %lli -> %lli\n", tf->srctent->tnode.tupid, tf->tent->tnode.tupid);
						tup_db_print(tf->f, tf->srctent->tnode.tupid);
						tup_db_print(tf->f, tf->tent->tnode.tupid);
						return NULL;
					}
				} else if(rparen - var > 7 &&
					  strncmp(var, "CONFIG_", 7) == 0) {
					const char *atvar;
					struct tup_entry *tent;
					atvar = var+7;

					tent = tup_db_get_var(tf->variant, atvar, rparen-atvar, &e);
					if(!tent)
						return NULL;
					if(tent_tree_add_dup(&tf->input_root, tent) < 0)
						return NULL;
				} else if(rparen - var > 6 &&
					  strncmp(var, "globs ", 6) == 0) {
					if(eval_globs_function(tf, var + 6, rparen - (var + 6), &e) < 0)
						return NULL;
				} else if(rparen - var > 4 &&
					  strncmp(var, "abs ", 4) == 0) {
					if(eval_abs_function(tf, var + 4, rparen - (var + 4), &e) < 0)
						return NULL;
				} else if(rparen - var > 7 &&
					  strncmp(var, "groups ", 7) == 0) {
					if(eval_groups_function(tf, var + 7, rparen - (var + 7), &e) < 0)
						return NULL;
				} else if(rparen - var > 9 &&
					  strncmp(var, "realname ", 9) == 0) {
					if(eval_realname_function(tf, var + 9, rparen - (var + 9), &e) < 0)
						return NULL;
				} else {
					struct var_entry *ve = NULL;
					struct tup_func_frame *frame = tf->func_frame;

					while(frame) {
						ve = vardb_get(&frame->vars, var, rparen-var);
						if(ve)
							break;
						frame = frame->parent;
					}
					if(ve) {
						if(estring_append(&e, ve->value, ve->vallen) < 0)
							return NULL;
					} else {
						if(luadb_copy(var, rparen-var, &e) < 0)
							return NULL;
					}
				}
				s = rparen + 1;
			} else {
				if(estring_append(&e, s, 1) < 0)
					return NULL;
				s++;
			}
		} else if(*s == '@') {
			const char *rparen;
			struct tup_entry *tent;

			if(s[1] == '(') {
				rparen = find_matching_paren(s+2);
				if(!rparen) {
					syntax_msg = "expected ending variable paren ')'";
					goto syntax_error;
				}

				var = s + 2;
				tent = tup_db_get_var(tf->variant, var, rparen-var, &e);
				if(!tent)
					return NULL;
				if(tent_tree_add_dup(&tf->input_root, tent) < 0)
					return NULL;
				s = rparen + 1;
			} else {
				if(estring_append(&e, s, 1) < 0)
					return NULL;
				s++;
			}
		} else if(*s == '&') {
			const char *rparen;

			if(s[1] == '(') {
				rparen = find_matching_paren(s+2);
				if(!rparen) {
					syntax_msg = "expected ending variable paren ')'";
					goto syntax_error;
				}

				var = s + 2;
				if(vardb_copy(&tf->node_db, var, rparen-var, &e) < 0)
					return NULL;
				s = rparen + 1;
			} else {
				if(estring_append(&e, s, 1) < 0)
					return NULL;
				s++;
			}
		} else {
			if(estring_append(&e, s, 1) < 0)
				return NULL;
			s++;
		}
	}
	if(estring_append(&e, s, strlen(s)) < 0)
		return NULL;


	char *rc = e.s;
	if(expand_nodes != EXPAND_NODES_CMD) {
		rc = eval_backticks(tf, e.s);
		free(e.s);
		if(!rc)
			return NULL;
	}
	if(expand_nodes != KEEP_NODES) {
		char *expanded = expand_node_strings(tf, rc, expand_nodes);
		free(rc);
		rc = expanded;
	}
	return rc;

syntax_error:
	fprintf(tf->f, "Syntax error: %s\n", syntax_msg);
	return NULL;
}
