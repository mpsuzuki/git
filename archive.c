#include "cache.h"
#include "config.h"
#include "refs.h"
#include "commit.h"
#include "tree-walk.h"
#include "attr.h"
#include "archive.h"
#include "parse-options.h"
#include "unpack-trees.h"
#include "dir.h"
#include "tar.h"

static char const * const archive_usage[] = {
	N_("git archive [<options>] <tree-ish> [<path>...]"),
	N_("git archive --list"),
	N_("git archive --remote <repo> [--exec <cmd>] [<options>] <tree-ish> [<path>...]"),
	N_("git archive --remote <repo> [--exec <cmd>] --list"),
	NULL
};

static const struct archiver **archivers;
static int nr_archivers;
static int alloc_archivers;
static int remote_allow_unreachable;

void register_archiver(struct archiver *ar)
{
	ALLOC_GROW(archivers, nr_archivers + 1, alloc_archivers);
	archivers[nr_archivers++] = ar;
}

static void format_subst(const struct commit *commit,
                         const char *src, size_t len,
                         struct strbuf *buf)
{
	char *to_free = NULL;
	struct strbuf fmt = STRBUF_INIT;
	struct pretty_print_context ctx = {0};
	ctx.date_mode.type = DATE_NORMAL;
	ctx.abbrev = DEFAULT_ABBREV;

	if (src == buf->buf)
		to_free = strbuf_detach(buf, NULL);
	for (;;) {
		const char *b, *c;

		b = memmem(src, len, "$Format:", 8);
		if (!b)
			break;
		c = memchr(b + 8, '$', (src + len) - b - 8);
		if (!c)
			break;

		strbuf_reset(&fmt);
		strbuf_add(&fmt, b + 8, c - b - 8);

		strbuf_add(buf, src, b - src);
		format_commit_message(commit, fmt.buf, buf, &ctx);
		len -= c + 1 - src;
		src  = c + 1;
	}
	strbuf_add(buf, src, len);
	strbuf_release(&fmt);
	free(to_free);
}

void *sha1_file_to_archive(const struct archiver_args *args,
			   const char *path, const unsigned char *sha1,
			   unsigned int mode, enum object_type *type,
			   unsigned long *sizep)
{
	void *buffer;
	const struct commit *commit = args->convert ? args->commit : NULL;

	path += args->baselen;
	buffer = read_sha1_file(sha1, type, sizep);
	if (buffer && S_ISREG(mode)) {
		struct strbuf buf = STRBUF_INIT;
		size_t size = 0;

		strbuf_attach(&buf, buffer, *sizep, *sizep + 1);
		convert_to_working_tree(path, buf.buf, buf.len, &buf);
		if (commit)
			format_subst(commit, buf.buf, buf.len, &buf);
		buffer = strbuf_detach(&buf, &size);
		*sizep = size;
	}

	return buffer;
}

struct directory {
	struct directory *up;
	struct object_id oid;
	int baselen, len;
	unsigned mode;
	int stage;
	char path[FLEX_ARRAY];
};

struct archiver_context {
	struct archiver_args *args;
	write_archive_entry_fn_t write_entry;
	struct directory *bottom;
};

static const struct attr_check *get_archive_attrs(const char *path)
{
	static struct attr_check *check;
	if (!check)
		check = attr_check_initl("export-ignore", "export-subst", NULL);
	return git_check_attr(path, check) ? NULL : check;
}

static int check_attr_export_ignore(const struct attr_check *check)
{
	return check && ATTR_TRUE(check->items[0].value);
}

static int check_attr_export_subst(const struct attr_check *check)
{
	return check && ATTR_TRUE(check->items[1].value);
}

static int write_archive_entry(const unsigned char *sha1, const char *base,
		int baselen, const char *filename, unsigned mode, int stage,
		void *context)
{
	static struct strbuf path = STRBUF_INIT;
	struct archiver_context *c = context;
	struct archiver_args *args = c->args;
	write_archive_entry_fn_t write_entry = c->write_entry;
	int err;
	const char *path_without_prefix;

	args->convert = 0;
	strbuf_reset(&path);
	strbuf_grow(&path, PATH_MAX);
	strbuf_add(&path, args->base, args->baselen);
	strbuf_add(&path, base, baselen);
	strbuf_addstr(&path, filename);
	if (S_ISDIR(mode) || S_ISGITLINK(mode))
		strbuf_addch(&path, '/');
	path_without_prefix = path.buf + args->baselen;

	if (!S_ISDIR(mode)) {
		const struct attr_check *check;
		check = get_archive_attrs(path_without_prefix);
		if (check_attr_export_ignore(check))
			return 0;
		args->convert = check_attr_export_subst(check);
	}

	if (S_ISDIR(mode) || S_ISGITLINK(mode)) {
		if (args->verbose)
			fprintf(stderr, "%.*s\n", (int)path.len, path.buf);
		err = write_entry(args, sha1, path.buf, path.len, mode);
		if (err)
			return err;
		return (S_ISDIR(mode) ? READ_TREE_RECURSIVE : 0);
	}

	if (args->verbose)
		fprintf(stderr, "%.*s\n", (int)path.len, path.buf);
	return write_entry(args, sha1, path.buf, path.len, mode);
}

static void queue_directory(const unsigned char *sha1,
		struct strbuf *base, const char *filename,
		unsigned mode, int stage, struct archiver_context *c)
{
	struct directory *d;
	size_t len = st_add4(base->len, 1, strlen(filename), 1);
	d = xmalloc(st_add(sizeof(*d), len));
	d->up	   = c->bottom;
	d->baselen = base->len;
	d->mode	   = mode;
	d->stage   = stage;
	c->bottom  = d;
	d->len = xsnprintf(d->path, len, "%.*s%s/", (int)base->len, base->buf, filename);
	hashcpy(d->oid.hash, sha1);
}

static int write_directory(struct archiver_context *c)
{
	struct directory *d = c->bottom;
	int ret;

	if (!d)
		return 0;
	c->bottom = d->up;
	d->path[d->len - 1] = '\0'; /* no trailing slash */
	ret =
		write_directory(c) ||
		write_archive_entry(d->oid.hash, d->path, d->baselen,
				    d->path + d->baselen, d->mode,
				    d->stage, c) != READ_TREE_RECURSIVE;
	free(d);
	return ret ? -1 : 0;
}

static int queue_or_write_archive_entry(const unsigned char *sha1,
		struct strbuf *base, const char *filename,
		unsigned mode, int stage, void *context)
{
	struct archiver_context *c = context;

	while (c->bottom &&
	       !(base->len >= c->bottom->len &&
		 !strncmp(base->buf, c->bottom->path, c->bottom->len))) {
		struct directory *next = c->bottom->up;
		free(c->bottom);
		c->bottom = next;
	}

	if (S_ISDIR(mode)) {
		size_t baselen = base->len;
		const struct attr_check *check;

		/* Borrow base, but restore its original value when done. */
		strbuf_addstr(base, filename);
		strbuf_addch(base, '/');
		check = get_archive_attrs(base->buf);
		strbuf_setlen(base, baselen);

		if (check_attr_export_ignore(check))
			return 0;
		queue_directory(sha1, base, filename,
				mode, stage, c);
		return READ_TREE_RECURSIVE;
	}

	if (write_directory(c))
		return -1;
	return write_archive_entry(sha1, base->buf, base->len, filename, mode,
				   stage, context);
}

int write_archive_entries(struct archiver_args *args,
		write_archive_entry_fn_t write_entry)
{
	struct archiver_context context;
	struct unpack_trees_options opts;
	struct tree_desc t;
	int err;

	if (args->baselen > 0 && args->base[args->baselen - 1] == '/') {
		size_t len = args->baselen;

		while (len > 1 && args->base[len - 2] == '/')
			len--;
		if (args->verbose)
			fprintf(stderr, "%.*s\n", (int)len, args->base);
		err = write_entry(args, args->tree->object.oid.hash, args->base,
				  len, 040777);
		if (err)
			return err;
	}

	memset(&context, 0, sizeof(context));
	context.args = args;
	context.write_entry = write_entry;

	/*
	 * Setup index and instruct attr to read index only
	 */
	if (!args->worktree_attributes) {
		memset(&opts, 0, sizeof(opts));
		opts.index_only = 1;
		opts.head_idx = -1;
		opts.src_index = &the_index;
		opts.dst_index = &the_index;
		opts.fn = oneway_merge;
		init_tree_desc(&t, args->tree->buffer, args->tree->size);
		if (unpack_trees(1, &t, &opts))
			return -1;
		git_attr_set_direction(GIT_ATTR_INDEX, &the_index);
	}

	err = read_tree_recursive(args->tree, "", 0, 0, &args->pathspec,
				  queue_or_write_archive_entry,
				  &context);
	if (err == READ_TREE_RECURSIVE)
		err = 0;
	while (context.bottom) {
		struct directory *next = context.bottom->up;
		free(context.bottom);
		context.bottom = next;
	}
	return err;
}

static const struct archiver *lookup_archiver(const char *name)
{
	int i;

	if (!name)
		return NULL;

	for (i = 0; i < nr_archivers; i++) {
		if (!strcmp(name, archivers[i]->name))
			return archivers[i];
	}
	return NULL;
}

static int reject_entry(const unsigned char *sha1, struct strbuf *base,
			const char *filename, unsigned mode,
			int stage, void *context)
{
	int ret = -1;
	if (S_ISDIR(mode)) {
		struct strbuf sb = STRBUF_INIT;
		strbuf_addbuf(&sb, base);
		strbuf_addstr(&sb, filename);
		if (!match_pathspec(context, sb.buf, sb.len, 0, NULL, 1))
			ret = READ_TREE_RECURSIVE;
		strbuf_release(&sb);
	}
	return ret;
}

static int path_exists(struct tree *tree, const char *path)
{
	const char *paths[] = { path, NULL };
	struct pathspec pathspec;
	int ret;

	parse_pathspec(&pathspec, 0, 0, "", paths);
	pathspec.recursive = 1;
	ret = read_tree_recursive(tree, "", 0, 0, &pathspec,
				  reject_entry, &pathspec);
	clear_pathspec(&pathspec);
	return ret != 0;
}

static void parse_pathspec_arg(const char **pathspec,
		struct archiver_args *ar_args)
{
	/*
	 * must be consistent with parse_pathspec in path_exists()
	 * Also if pathspec patterns are dependent, we're in big
	 * trouble as we test each one separately
	 */
	parse_pathspec(&ar_args->pathspec, 0,
		       PATHSPEC_PREFER_FULL,
		       "", pathspec);
	ar_args->pathspec.recursive = 1;
	if (pathspec) {
		while (*pathspec) {
			if (**pathspec && !path_exists(ar_args->tree, *pathspec))
				die(_("pathspec '%s' did not match any files"), *pathspec);
			pathspec++;
		}
	}
}

static void parse_treeish_arg(const char **argv,
		struct archiver_args *ar_args, const char *prefix,
		int remote)
{
	const char *name = argv[0];
	const unsigned char *commit_sha1;
	time_t archive_time;
	struct tree *tree;
	const struct commit *commit;
	struct object_id oid;

	/* Remotes are only allowed to fetch actual refs */
	if (remote && !remote_allow_unreachable) {
		char *ref = NULL;
		const char *colon = strchrnul(name, ':');
		int refnamelen = colon - name;

		if (!dwim_ref(name, refnamelen, &oid, &ref))
			die("no such ref: %.*s", refnamelen, name);
		free(ref);
	}

	if (get_oid(name, &oid))
		die("Not a valid object name");

	commit = lookup_commit_reference_gently(&oid, 1);
	if (commit) {
		commit_sha1 = commit->object.oid.hash;
		archive_time = commit->date;
	} else {
		commit_sha1 = NULL;
		archive_time = time(NULL);
	}

	tree = parse_tree_indirect(&oid);
	if (tree == NULL)
		die("not a tree object");

	if (prefix) {
		struct object_id tree_oid;
		unsigned int mode;
		int err;

		err = get_tree_entry(tree->object.oid.hash, prefix,
				     tree_oid.hash, &mode);
		if (err || !S_ISDIR(mode))
			die("current working directory is untracked");

		tree = parse_tree_indirect(&tree_oid);
	}
	ar_args->tree = tree;
	ar_args->commit_sha1 = commit_sha1;
	ar_args->commit = commit;
	ar_args->time = archive_time;
}

#define OPT__COMPR(s, v, h, p) \
	{ OPTION_SET_INT, (s), NULL, (v), NULL, (h), \
	  PARSE_OPT_NOARG | PARSE_OPT_NONEG, NULL, (p) }
#define OPT__COMPR_HIDDEN(s, v, p) \
	{ OPTION_SET_INT, (s), NULL, (v), NULL, "", \
	  PARSE_OPT_NOARG | PARSE_OPT_NONEG | PARSE_OPT_HIDDEN, NULL, (p) }

/*
 * --owner, --group options reject hexdigit, signed int values.
 * strtol(), atoi() are too permissive to similate this behaviour.
 */
#define STR_IS_DIGIT_OK 0
#define STR_IS_NOT_DIGIT -1
#define STR_IS_DIGIT_TOO_LARGE -2

static int try_as_simple_digit(const char *s, unsigned long *dst)
{
	unsigned long ul;
	char *endptr;

	if (strlen(s) != strspn(s, "0123456789"))
		return STR_IS_NOT_DIGIT;

	errno = 0;
	ul = strtoul(s, &endptr, 10);

	/* catch ERANGE */
	if (errno) {
		errno = 0;
		return STR_IS_DIGIT_TOO_LARGE;
	}

#if ULONG_MAX > 0xFFFFFFFFUL
	/*
	 * --owner, --group rejects uid/gid greater than 32-bit
	 * limits, even on 64-bit platforms.
	 */
	if (ul > 0xFFFFFFFFUL)
		return STR_IS_DIGIT_TOO_LARGE;
#endif

	if (dst)
		*dst = ul;
	return STR_IS_DIGIT_OK;
}

static const char* get_whole_or_substr_after_colon(const char *s)
{
	const char *col_pos;

	col_pos = strchr(s, ':');
	if (!col_pos)
		return s;

	return (col_pos + 1);
}

#define STR_IS_NAME_COLON_DIGIT 0
#define STR_HAS_NO_COLON -1
#define STR_HAS_DIGIT_BROKEN -2
#define STR_HAS_DIGIT_TOO_LARGE -3

static int try_as_name_colon_digit(const char *s, const char **dst_s,
		unsigned long *dst_ul)
{
	int r;
	const char *t;

	t = get_whole_or_substr_after_colon(s);
	if (t == s)
		return STR_HAS_NO_COLON;

	r = try_as_simple_digit(t, dst_ul);
	switch (r) {
	case STR_IS_DIGIT_OK:
		*dst_s = xstrndup(s, t-s-1);
		return STR_IS_NAME_COLON_DIGIT;
	case STR_IS_DIGIT_TOO_LARGE:
		return STR_HAS_DIGIT_TOO_LARGE;
	default:
		return STR_HAS_DIGIT_BROKEN;
	}
}

#define NAME_ID_GIVEN_BOTH 0
#define NAME_ID_ID_GUESSED 1
#define NAME_ID_ID_UNTOUCHED 2
#define NAME_ID_NAME_GUESSED 3
#define NAME_ID_NAME_EMPTY 4
#define NAME_ID_ERR_ID_TOO_LARGE -126
#define NAME_ID_ERR_SYNTAX -127
#define NAME_ID_ERR_PARAMS -128

static int set_args_uname_uid(struct archiver_args *args,
		const char *tar_owner)
{
	int r;
	struct passwd *pw = NULL;

	if (!args || !tar_owner)
		return NAME_ID_ERR_PARAMS;

	r = try_as_name_colon_digit(tar_owner, &(args->uname),
				    &(args->uid));
	switch (r) {
	case STR_IS_NAME_COLON_DIGIT:
		return NAME_ID_GIVEN_BOTH;
	case STR_HAS_DIGIT_TOO_LARGE:
		return NAME_ID_ERR_ID_TOO_LARGE;
	case STR_HAS_DIGIT_BROKEN:
		return NAME_ID_ERR_SYNTAX;
	}

	/* in below, the operand consists of 1 token */

	r = try_as_simple_digit(tar_owner, &(args->uid));
	switch (r) {
	case STR_IS_DIGIT_TOO_LARGE:
		return NAME_ID_ERR_ID_TOO_LARGE;
	case STR_IS_DIGIT_OK:
		pw = getpwuid(args->uid);
		if (!pw) {
			args->uname = xstrdup("");
			return NAME_ID_NAME_EMPTY;
		}
		args->uname = xstrdup(pw->pw_name);
		return NAME_ID_NAME_GUESSED;
	}

	/* the operand is not digit, take it as username */

	args->uname = xstrdup(tar_owner);
	pw = getpwnam(tar_owner);
	if (!pw)
		return NAME_ID_ID_UNTOUCHED;
	args->uid = pw->pw_uid;
	return NAME_ID_ID_GUESSED;
}

static int set_args_gname_gid(struct archiver_args *args,
		const char *tar_group)
{
	int r;
	struct group *gr = NULL;

	if (!args || !tar_group)
		return NAME_ID_ERR_PARAMS;

	r = try_as_name_colon_digit(tar_group, &(args->gname),
				    &(args->gid));
	switch (r) {
	case STR_IS_NAME_COLON_DIGIT:
		return NAME_ID_GIVEN_BOTH;
	case STR_HAS_DIGIT_TOO_LARGE:
		return NAME_ID_ERR_ID_TOO_LARGE;
	case STR_HAS_DIGIT_BROKEN:
		return NAME_ID_ERR_SYNTAX;
	}

	/* in below, the operand consists of 1 token */

	r = try_as_simple_digit(tar_group, &(args->gid));
	switch (r) {
	case STR_IS_DIGIT_TOO_LARGE:
		return NAME_ID_ERR_ID_TOO_LARGE;
	case STR_IS_DIGIT_OK:
		gr = getgrgid(args->gid);
		if (!gr) {
			args->gname = xstrdup("");
			return NAME_ID_NAME_EMPTY;
		}
		args->gname = xstrdup(gr->gr_name);
		return NAME_ID_NAME_GUESSED;
	}

	/* the operand is not digit, take it as groupname */

	args->gname = xstrdup(tar_group);
	gr = getgrnam(tar_group);
	if (!gr)
		return NAME_ID_ID_UNTOUCHED;
	args->gid = gr->gr_gid;
	return NAME_ID_ID_GUESSED;
}

static void set_args_tar_owner_group(struct archiver_args *args,
	const char *tar_owner, const char *tar_group)
{
	int r;

	/* initialize by default values */
	args->uname = xstrdup("root");
	args->gname = xstrdup("root");
	args->uid = 0;
	args->gid = 0;

	/*
	 * GNU tar --format=ustar checks if uid is in 0..209751.
	 * Too long digit string could not be dealt as numeric,
	 * it is rejected as a syntax error before range check.
	 */
	r = set_args_uname_uid(args, tar_owner);
	switch (r) {
	case NAME_ID_ERR_ID_TOO_LARGE:
	case NAME_ID_ERR_SYNTAX:
		die("'%s': Invalid owner ID",
		    get_whole_or_substr_after_colon(tar_owner));
	}
	if (args->uid > MAX_ID_IN_TAR_US)
		die("value %ld out of uid_t range 0..%ld", args->uid,
		     MAX_ID_IN_TAR_US);

	r = set_args_gname_gid(args, tar_group);
	switch (r) {
	case NAME_ID_ERR_ID_TOO_LARGE:
	case NAME_ID_ERR_SYNTAX:
		die("'%s': Invalid group ID",
		    get_whole_or_substr_after_colon(tar_group));
	}
	if (args->gid > MAX_ID_IN_TAR_US)
		die("value %ld out of gid_t range 0..%ld", args->gid,
		    MAX_ID_IN_TAR_US);
}

static int parse_archive_args(int argc, const char **argv,
		const struct archiver **ar, struct archiver_args *args,
		const char *name_hint, int is_remote)
{
	const char *format = NULL;
	const char *base = NULL;
	const char *remote = NULL;
	const char *exec = NULL;
	const char *output = NULL;
	int compression_level = -1;
	int verbose = 0;
	int i;
	int list = 0;
	int worktree_attributes = 0;
	const char *tar_owner = NULL;
	const char *tar_group = NULL;
	struct option opts[] = {
		OPT_GROUP(""),
		OPT_STRING(0, "format", &format, N_("fmt"), N_("archive format")),
		OPT_STRING(0, "prefix", &base, N_("prefix"),
			N_("prepend prefix to each pathname in the archive")),
		OPT_STRING('o', "output", &output, N_("file"),
			N_("write the archive to this file")),
		OPT_BOOL(0, "worktree-attributes", &worktree_attributes,
			N_("read .gitattributes in working directory")),
		OPT__VERBOSE(&verbose, N_("report archived files on stderr")),
		OPT__COMPR('0', &compression_level, N_("store only"), 0),
		OPT__COMPR('1', &compression_level, N_("compress faster"), 1),
		OPT__COMPR_HIDDEN('2', &compression_level, 2),
		OPT__COMPR_HIDDEN('3', &compression_level, 3),
		OPT__COMPR_HIDDEN('4', &compression_level, 4),
		OPT__COMPR_HIDDEN('5', &compression_level, 5),
		OPT__COMPR_HIDDEN('6', &compression_level, 6),
		OPT__COMPR_HIDDEN('7', &compression_level, 7),
		OPT__COMPR_HIDDEN('8', &compression_level, 8),
		OPT__COMPR('9', &compression_level, N_("compress better"), 9),
		OPT_GROUP(""),
		OPT_BOOL('l', "list", &list,
			N_("list supported archive formats")),
		OPT_GROUP(""),
		OPT_STRING(0, "remote", &remote, N_("repo"),
			N_("retrieve the archive from remote repository <repo>")),
		OPT_STRING(0, "exec", &exec, N_("command"),
			N_("path to the remote git-upload-archive command")),
		OPT_STRING(0, "owner", &tar_owner, N_("owner"), N_("<name[:uid]> in tar")),
		OPT_STRING(0, "group", &tar_group, N_("group"), N_("<name[:gid]> in tar")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, opts, archive_usage, 0);

	if (remote)
		die(_("Unexpected option --remote"));
	if (exec)
		die(_("Option --exec can only be used together with --remote"));
	if (output)
		die(_("Unexpected option --output"));

	if (!base)
		base = "";

	if (list) {
		for (i = 0; i < nr_archivers; i++)
			if (!is_remote || archivers[i]->flags & ARCHIVER_REMOTE)
				printf("%s\n", archivers[i]->name);
		exit(0);
	}

	if (!format && name_hint)
		format = archive_format_from_filename(name_hint);
	if (!format)
		format = "tar";

	/* We need at least one parameter -- tree-ish */
	if (argc < 1)
		usage_with_options(archive_usage, opts);
	*ar = lookup_archiver(format);
	if (!*ar || (is_remote && !((*ar)->flags & ARCHIVER_REMOTE)))
		die(_("Unknown archive format '%s'"), format);

	args->compression_level = Z_DEFAULT_COMPRESSION;
	if (compression_level != -1) {
		if ((*ar)->flags & ARCHIVER_WANT_COMPRESSION_LEVELS)
			args->compression_level = compression_level;
		else {
			die(_("Argument not supported for format '%s': -%d"),
					format, compression_level);
		}
	}
	args->verbose = verbose;
	args->base = base;
	args->baselen = strlen(base);
	args->worktree_attributes = worktree_attributes;

	set_args_tar_owner_group(args, tar_owner, tar_group);

	return argc;
}

int write_archive(int argc, const char **argv, const char *prefix,
		  const char *name_hint, int remote)
{
	const struct archiver *ar = NULL;
	struct archiver_args args;

	git_config_get_bool("uploadarchive.allowunreachable", &remote_allow_unreachable);
	git_config(git_default_config, NULL);

	init_tar_archiver();
	init_zip_archiver();

	argc = parse_archive_args(argc, argv, &ar, &args, name_hint, remote);
	if (!startup_info->have_repository) {
		/*
		 * We know this will die() with an error, so we could just
		 * die ourselves; but its error message will be more specific
		 * than what we could write here.
		 */
		setup_git_directory();
	}

	parse_treeish_arg(argv, &args, prefix, remote);
	parse_pathspec_arg(argv + 1, &args);

	return ar->write_archive(ar, &args);
}

static int match_extension(const char *filename, const char *ext)
{
	int prefixlen = strlen(filename) - strlen(ext);

	/*
	 * We need 1 character for the '.', and 1 character to ensure that the
	 * prefix is non-empty (k.e., we don't match .tar.gz with no actual
	 * filename).
	 */
	if (prefixlen < 2 || filename[prefixlen - 1] != '.')
		return 0;
	return !strcmp(filename + prefixlen, ext);
}

const char *archive_format_from_filename(const char *filename)
{
	int i;

	for (i = 0; i < nr_archivers; i++)
		if (match_extension(filename, archivers[i]->name))
			return archivers[i]->name;
	return NULL;
}
