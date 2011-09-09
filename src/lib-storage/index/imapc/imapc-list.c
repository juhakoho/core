/* Copyright (c) 2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "imap-arg.h"
#include "imap-match.h"
#include "imap-utf7.h"
#include "mailbox-tree.h"
#include "mailbox-list-subscriptions.h"
#include "imapc-client.h"
#include "imapc-storage.h"
#include "imapc-list.h"

struct imapc_mailbox_list_iterate_context {
	struct mailbox_list_iterate_context ctx;
	struct mailbox_tree_context *tree;
	struct mailbox_tree_iterate_context *iter;
	struct mailbox_info info;
};

extern struct mailbox_list imapc_mailbox_list;

static struct mailbox_list *imapc_list_alloc(void)
{
	struct imapc_mailbox_list *list;
	pool_t pool;

	pool = pool_alloconly_create("imapc mailbox list", 1024);
	list = p_new(pool, struct imapc_mailbox_list, 1);
	list->list = imapc_mailbox_list;
	list->list.pool = pool;
	return &list->list;
}

static void imapc_list_deinit(struct mailbox_list *_list)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;

	if (list->mailboxes != NULL)
		mailbox_tree_deinit(&list->mailboxes);
	if (list->tmp_subscriptions != NULL)
		mailbox_tree_deinit(&list->tmp_subscriptions);
	pool_unref(&list->list.pool);
}

static void imapc_list_simple_callback(const struct imapc_command_reply *reply,
				       void *context)
{
	struct imapc_simple_context *ctx = context;
	const char *str;
	enum mail_error error;

	imapc_simple_callback(reply, context);
	if (ctx->ret < 0) {
		str = mail_storage_get_last_error(&ctx->storage->storage, &error);
		mailbox_list_set_error(&ctx->storage->list->list, error, str);
	}
}

static struct mailbox_node *
imapc_list_update_tree(struct mailbox_tree_context *tree,
		       const struct imap_arg *args)
{
	struct mailbox_node *node;
	const struct imap_arg *flags;
	const char *name, *flag;
	enum mailbox_info_flags info_flags = 0;
	bool created;

	if (!imap_arg_get_list(&args[0], &flags) ||
	    args[1].type == IMAP_ARG_EOL ||
	    !imap_arg_get_astring(&args[2], &name))
		return NULL;

	while (imap_arg_get_atom(flags, &flag)) {
		if (strcasecmp(flag, "\\NoSelect") == 0)
			info_flags |= MAILBOX_NOSELECT;
		else if (strcasecmp(flag, "\\NonExistent") == 0)
			info_flags |= MAILBOX_NONEXISTENT;
		else if (strcasecmp(flag, "\\NoInferiors") == 0)
			info_flags |= MAILBOX_NOINFERIORS;
		else if (strcasecmp(flag, "\\Subscribed") == 0)
			info_flags |= MAILBOX_SUBSCRIBED;
		flags++;
	}

	T_BEGIN {
		string_t *utf8_name = t_str_new(64);

		if (imap_utf7_to_utf8(name, utf8_name)) {
			str_truncate(utf8_name, 0);
			str_append(utf8_name, name);
		}
		if ((info_flags & MAILBOX_NONEXISTENT) != 0)
			node = mailbox_tree_lookup(tree, str_c(utf8_name));
		else {
			node = mailbox_tree_get(tree, str_c(utf8_name),
						&created);
		}
	} T_END;
	if (node != NULL)
		node->flags = info_flags;
	return node;
}

static void imapc_untagged_list(const struct imapc_untagged_reply *reply,
				struct imapc_storage *storage)
{
	struct imapc_mailbox_list *list = storage->list;
	const struct imap_arg *args = reply->args;
	const char *sep, *name;

	if (list->sep == '\0') {
		/* we haven't asked for the separator yet.
		   lets see if this is the reply for its request. */
		if (args[0].type == IMAP_ARG_EOL ||
		    !imap_arg_get_nstring(&args[1], &sep) ||
		    !imap_arg_get_astring(&args[2], &name))
			return;

		/* we can't handle NIL separator yet */
		list->sep = sep == NULL ? '/' : sep[0];
		if (list->mailboxes != NULL)
			mailbox_tree_set_separator(list->mailboxes, list->sep);
	} else {
		(void)imapc_list_update_tree(list->mailboxes, args);
	}
}

static void imapc_untagged_lsub(const struct imapc_untagged_reply *reply,
				struct imapc_storage *storage)
{
	struct imapc_mailbox_list *list = storage->list;
	const struct imap_arg *args = reply->args;
	struct mailbox_node *node;

	if (list->sep == '\0') {
		/* we haven't asked for the separator yet */
		return;
	}
	node = imapc_list_update_tree(list->tmp_subscriptions != NULL ?
				      list->tmp_subscriptions :
				      list->list.subscriptions, args);
	if (node != NULL)
		node->flags |= MAILBOX_SUBSCRIBED;
}

void imapc_list_register_callbacks(struct imapc_mailbox_list *list)
{
	imapc_storage_register_untagged(list->storage, "LIST",
					imapc_untagged_list);
	imapc_storage_register_untagged(list->storage, "LSUB",
					imapc_untagged_lsub);
}

static bool
imapc_is_valid_pattern(struct mailbox_list *list ATTR_UNUSED,
		       const char *pattern ATTR_UNUSED)
{
	return TRUE;
}

static bool
imapc_is_valid_existing_name(struct mailbox_list *list ATTR_UNUSED,
			     const char *name ATTR_UNUSED)
{
	return TRUE;
}

static bool
imapc_is_valid_create_name(struct mailbox_list *list ATTR_UNUSED,
			   const char *name ATTR_UNUSED)
{
	return TRUE;
}

static char imapc_list_get_hierarchy_sep(struct mailbox_list *_list)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;

	/* storage should have looked this up when it was created */
	i_assert(list->sep != '\0');

	return list->sep;
}

static struct mailbox_list *imapc_list_get_fs(struct imapc_mailbox_list *list)
{
	struct mailbox_list_settings list_set;
	const char *error, *dir;

	dir = list->list.set.index_dir;
	if (dir == NULL)
		dir = list->list.set.root_dir;

	if (dir == NULL) {
		/* indexes disabled */
	} else if (list->index_list == NULL && !list->index_list_failed) {
		memset(&list_set, 0, sizeof(list_set));
		list_set.layout = MAILBOX_LIST_NAME_MAILDIRPLUSPLUS;
		/* the root dir shouldn't actually ever be used. we just need
		   it to be different from index_dir so the index directories
		   get autocreated */
		list_set.root_dir = dir;
		list_set.index_dir = t_strconcat(dir, "/indexes", NULL);
		list_set.escape_char = '%';

		if (mailbox_list_create(list_set.layout, list->list.ns,
					&list_set, MAILBOX_LIST_FLAG_SECONDARY,
					&list->index_list, &error) < 0) {
			i_error("imapc: Couldn't create %s mailbox list: %s",
				list_set.layout, error);
			list->index_list_failed = TRUE;
		}
	}
	return list->index_list;
}

static const char *
imapc_list_get_fs_name(struct imapc_mailbox_list *list, const char *name)
{
	struct mailbox_list *fs_list = imapc_list_get_fs(list);
	const char *vname;

	if (name == NULL)
		return name;

	vname = mailbox_list_get_vname(&list->list, name);
	return mailbox_list_get_storage_name(fs_list, vname);
}

static const char *
imapc_list_get_path(struct mailbox_list *_list, const char *name,
		    enum mailbox_list_path_type type)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;
	struct mailbox_list *fs_list = imapc_list_get_fs(list);
	const char *fs_name;

	if (fs_list != NULL) {
		fs_name = imapc_list_get_fs_name(list, name);
		return mailbox_list_get_path(fs_list, fs_name, type);
	} else {
		if (type == MAILBOX_LIST_PATH_TYPE_INDEX)
			return "";
		return NULL;
	}
}

static const char *
imapc_list_get_temp_prefix(struct mailbox_list *_list, bool global)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;
	struct mailbox_list *fs_list = imapc_list_get_fs(list);

	if (fs_list != NULL) {
		return global ?
			mailbox_list_get_global_temp_prefix(fs_list) :
			mailbox_list_get_temp_prefix(fs_list);
	} else {
		i_panic("imapc: Can't return a temp prefix for '%s'",
			_list->ns->prefix);
		return NULL;
	}
}

static const char *
imapc_list_join_refpattern(struct mailbox_list *list ATTR_UNUSED,
			   const char *ref, const char *pattern)
{
	return t_strconcat(ref, pattern, NULL);
}

static int imapc_list_refresh(struct imapc_mailbox_list *list)
{
	struct imapc_simple_context ctx;

	if (list->refreshed_mailboxes)
		return 0;

	if (list->sep == '\0')
		(void)mailbox_list_get_hierarchy_sep(&list->list);

	imapc_simple_context_init(&ctx, list->storage);
	imapc_client_cmdf(list->storage->client,
			  imapc_list_simple_callback, &ctx, "LIST \"\" *");
	if (list->mailboxes != NULL)
		mailbox_tree_deinit(&list->mailboxes);
	list->mailboxes = mailbox_tree_init(list->sep);

	imapc_simple_run(&ctx);
	if (ctx.ret == 0)
		list->refreshed_mailboxes = TRUE;
	return ctx.ret;
}

static void
imapc_list_build_match_tree(struct imapc_mailbox_list_iterate_context *ctx)
{
	struct imapc_mailbox_list *list =
		(struct imapc_mailbox_list *)ctx->ctx.list;
	struct mailbox_list_iter_update_context update_ctx;
	struct mailbox_tree_iterate_context *iter;
	struct mailbox_node *node;
	const char *name;

	memset(&update_ctx, 0, sizeof(update_ctx));
	update_ctx.iter_ctx = &ctx->ctx;
	update_ctx.tree_ctx = ctx->tree;
	update_ctx.glob = ctx->ctx.glob;
	update_ctx.match_parents = TRUE;

	iter = mailbox_tree_iterate_init(list->mailboxes, NULL, 0);
	while ((node = mailbox_tree_iterate_next(iter, &name)) != NULL) {
		update_ctx.leaf_flags = node->flags;
		mailbox_list_iter_update(&update_ctx, name);
	}
	mailbox_tree_iterate_deinit(&iter);
}

static struct mailbox_list_iterate_context *
imapc_list_iter_init(struct mailbox_list *_list, const char *const *patterns,
		     enum mailbox_list_iter_flags flags)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;
	struct mailbox_list_iterate_context *_ctx;
	struct imapc_mailbox_list_iterate_context *ctx;
	char sep;
	int ret = 0;

	if ((flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) == 0 ||
	    (flags & MAILBOX_LIST_ITER_RETURN_NO_FLAGS) == 0)
		ret = imapc_list_refresh(list);

	list->iter_count++;

	if ((flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0) {
		/* we're listing only subscriptions. just use the cached
		   subscriptions list. */
		_ctx = mailbox_list_subscriptions_iter_init(_list, patterns,
							    flags);
		if (ret < 0)
			_ctx->failed = TRUE;
		return _ctx;
	}

	sep = mailbox_list_get_hierarchy_sep(_list);

	ctx = i_new(struct imapc_mailbox_list_iterate_context, 1);
	ctx->ctx.list = _list;
	ctx->ctx.flags = flags;
	ctx->ctx.glob = imap_match_init_multiple(default_pool, patterns,
						 FALSE, sep);
	array_create(&ctx->ctx.module_contexts, default_pool, sizeof(void *), 5);

	ctx->info.ns = _list->ns;

	ctx->tree = mailbox_tree_init(sep);
	imapc_list_build_match_tree(ctx);
	ctx->iter = mailbox_tree_iterate_init(ctx->tree, NULL, 0);
	if (ret < 0)
		ctx->ctx.failed = TRUE;
	return &ctx->ctx;
}

static const struct mailbox_info *
imapc_list_iter_next(struct mailbox_list_iterate_context *_ctx)
{
	struct imapc_mailbox_list_iterate_context *ctx =
		(struct imapc_mailbox_list_iterate_context *)_ctx;
	struct mailbox_node *node;
	const char *name;

	if (_ctx->failed)
		return NULL;

	if ((_ctx->flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0)
		return mailbox_list_subscriptions_iter_next(_ctx);

	node = mailbox_tree_iterate_next(ctx->iter, &name);
	if (node == NULL)
		return NULL;

	ctx->info.name = name;
	ctx->info.flags = node->flags;
	return &ctx->info;
}

static int imapc_list_iter_deinit(struct mailbox_list_iterate_context *_ctx)
{
	struct imapc_mailbox_list_iterate_context *ctx =
		(struct imapc_mailbox_list_iterate_context *)_ctx;
	struct imapc_mailbox_list *list =
		(struct imapc_mailbox_list *)_ctx->list;
	int ret = _ctx->failed ? -1 : 0;

	i_assert(list->iter_count > 0);

	if (--list->iter_count == 0) {
		list->refreshed_mailboxes = FALSE;
		list->refreshed_subscriptions = FALSE;
	}

	if ((_ctx->flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0)
		return mailbox_list_subscriptions_iter_deinit(_ctx);

	mailbox_tree_iterate_deinit(&ctx->iter);
	mailbox_tree_deinit(&ctx->tree);
	imap_match_deinit(&_ctx->glob);
	array_free(&_ctx->module_contexts);
	i_free(ctx);
	return ret;
}

static int
imapc_list_subscriptions_refresh(struct mailbox_list *_src_list,
				 struct mailbox_list *dest_list)
{
	struct imapc_mailbox_list *src_list =
		(struct imapc_mailbox_list *)_src_list;
	struct imapc_simple_context ctx;
	char sep;

	i_assert(src_list->tmp_subscriptions == NULL);

	if (src_list->refreshed_subscriptions) {
		if (dest_list->subscriptions == NULL) {
			sep = mailbox_list_get_hierarchy_sep(dest_list);
			dest_list->subscriptions =
				mailbox_tree_init(sep);
		}
		return 0;
	}

	if (src_list->sep == '\0')
		(void)mailbox_list_get_hierarchy_sep(_src_list);

	src_list->tmp_subscriptions = mailbox_tree_init(src_list->sep);

	imapc_simple_context_init(&ctx, src_list->storage);
	imapc_client_cmdf(src_list->storage->client,
			  imapc_list_simple_callback, &ctx,
			  "LSUB \"\" *");
	imapc_simple_run(&ctx);

	/* replace subscriptions tree in destination */
	mailbox_tree_set_separator(src_list->tmp_subscriptions,
				   mailbox_list_get_hierarchy_sep(dest_list));
	if (dest_list->subscriptions != NULL)
		mailbox_tree_deinit(&dest_list->subscriptions);
	dest_list->subscriptions = src_list->tmp_subscriptions;
	src_list->tmp_subscriptions = NULL;

	src_list->refreshed_subscriptions = TRUE;
	return 0;
}

static int imapc_list_set_subscribed(struct mailbox_list *_list,
				     const char *name, bool set)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;
	struct imapc_simple_context ctx;

	imapc_simple_context_init(&ctx, list->storage);
	imapc_client_cmdf(list->storage->client,
			  imapc_list_simple_callback, &ctx,
			  set ? "SUBSCRIBE %s" : "UNSUBSCRIBE %s", name);
	imapc_simple_run(&ctx);
	return ctx.ret;
}

static int
imapc_list_create_mailbox_dir(struct mailbox_list *list ATTR_UNUSED,
			      const char *name ATTR_UNUSED,
			      enum mailbox_dir_create_type type ATTR_UNUSED)
{
	/* this gets called just before mailbox.create().
	   we don't need to do anything. */
	return 0;
}

static int
imapc_list_delete_mailbox(struct mailbox_list *_list, const char *name)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;
	struct imapc_simple_context ctx;

	imapc_simple_context_init(&ctx, list->storage);
	imapc_client_cmdf(list->storage->client,
			  imapc_list_simple_callback, &ctx, "DELETE %s", name);
	imapc_simple_run(&ctx);
	return ctx.ret;
}

static int
imapc_list_delete_dir(struct mailbox_list *_list, const char *name)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)_list;
	struct mailbox_list *fs_list = imapc_list_get_fs(list);

	if (fs_list != NULL) {
		name = imapc_list_get_fs_name(list, name);
		(void)mailbox_list_delete_dir(fs_list, name);
	}
	return 0;
}

static int
imapc_list_delete_symlink(struct mailbox_list *list,
			  const char *name ATTR_UNUSED)
{
	mailbox_list_set_error(list, MAIL_ERROR_NOTPOSSIBLE, "Not supported");
	return -1;
}

static int
imapc_list_rename_mailbox(struct mailbox_list *oldlist, const char *oldname,
			  struct mailbox_list *newlist, const char *newname,
			  bool rename_children)
{
	struct imapc_mailbox_list *list = (struct imapc_mailbox_list *)oldlist;
	struct mailbox_list *fs_list = imapc_list_get_fs(list);
	struct imapc_simple_context ctx;

	if (!rename_children) {
		mailbox_list_set_error(oldlist, MAIL_ERROR_NOTPOSSIBLE,
			"Renaming without children not supported.");
		return -1;
	}

	if (oldlist != newlist) {
		mailbox_list_set_error(oldlist, MAIL_ERROR_NOTPOSSIBLE,
			"Can't rename mailboxes across storages.");
		return -1;
	}

	imapc_simple_context_init(&ctx, list->storage);
	imapc_client_cmdf(list->storage->client,
			  imapc_list_simple_callback, &ctx,
			  "RENAME %s %s", oldname, newname);
	imapc_simple_run(&ctx);
	if (ctx.ret == 0 && fs_list != NULL && oldlist == newlist) {
		oldname = imapc_list_get_fs_name(list, oldname);
		newname = imapc_list_get_fs_name(list, newname);
		(void)fs_list->v.rename_mailbox(fs_list, oldname,
						fs_list, newname,
						rename_children);
	}
	return ctx.ret;
}

struct mailbox_list imapc_mailbox_list = {
	.name = MAILBOX_LIST_NAME_IMAPC,
	.props = MAILBOX_LIST_PROP_NO_ROOT,
	.mailbox_name_max_length = MAILBOX_LIST_NAME_MAX_LENGTH,

	{
		imapc_list_alloc,
		imapc_list_deinit,
		NULL,
		imapc_is_valid_pattern,
		imapc_is_valid_existing_name,
		imapc_is_valid_create_name,
		imapc_list_get_hierarchy_sep,
		mailbox_list_default_get_vname,
		mailbox_list_default_get_storage_name,
		imapc_list_get_path,
		imapc_list_get_temp_prefix,
		imapc_list_join_refpattern,
		imapc_list_iter_init,
		imapc_list_iter_next,
		imapc_list_iter_deinit,
		NULL,
		NULL,
		imapc_list_subscriptions_refresh,
		imapc_list_set_subscribed,
		imapc_list_create_mailbox_dir,
		imapc_list_delete_mailbox,
		imapc_list_delete_dir,
		imapc_list_delete_symlink,
		imapc_list_rename_mailbox
	}
};
