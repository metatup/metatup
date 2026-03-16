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

#ifndef tup_vardb_h
#define tup_vardb_h

#include "tupid.h"
#include "entry.h"
#include "estring.h"
#include "string_tree.h"
#include "tent_list.h"

struct tup_entry;

enum vardb_merge_mode {
	VDB_MERGE_INHERIT = -1,
	VDB_MERGE_OVERRIDE = 0,
	VDB_MERGE_APPEND = 1,
	VDB_MERGE_PREPEND = 2,
};

struct vardb {
	struct string_entries root;
	int count;
};

struct var_entry {
	struct string_tree var;
	char *value;
	int vallen;
	enum vardb_merge_mode merge_mode;
	struct tup_entry *tent;   /* only used in db.c */
};

int vardb_init(struct vardb *v);
int vardb_close(struct vardb *v);
int vardb_set(struct vardb *v, const char *var, const char *value,
              struct tup_entry *tent);
struct var_entry *vardb_set2(struct vardb *v, const char *var, int varlen,
                             const char *value, struct tup_entry *tent);
int vardb_set_mode(struct vardb *v, const char *var, const char *value,
		     enum vardb_merge_mode mode, struct tup_entry *tent);
struct var_entry *vardb_set2_mode(struct vardb *v, const char *var, int varlen,
				    const char *value, enum vardb_merge_mode mode,
				    struct tup_entry *tent);
int vardb_append(struct vardb *v, const char *var, const char *value);
int vardb_merge(struct vardb *v, const char *var, const char *value,
		 enum vardb_merge_mode mode);
int vardb_copy(struct vardb *v, const char *var, int varlen, struct estring *e);
struct var_entry *vardb_get(struct vardb *v, const char *var, int varlen);
int vardb_compare(struct vardb *vdba, struct vardb *vdbb,
		  int (*extra_a)(struct var_entry *ve, struct tup_entry *var_dtent),
		  int (*extra_b)(struct var_entry *ve, struct tup_entry *var_dtent),
		  int (*same)(struct var_entry *vea, struct var_entry *veb),
		  struct tup_entry *var_dtent);
void vardb_dump(struct vardb *v);

#endif
