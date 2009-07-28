/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "str.h"
#include "hex-binary.h"
#include "istream.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-search-build.h"
#include "dsync-worker-private.h"

struct local_dsync_worker_mailbox_iter {
	struct dsync_worker_mailbox_iter iter;
	struct mailbox_list_iterate_context *list_iter;
};

struct local_dsync_worker_msg_iter {
	struct dsync_worker_msg_iter iter;
	mailbox_guid_t *mailboxes;
	unsigned int mailbox_idx, mailbox_count;

	struct mail_search_context *search_ctx;
	struct mail *mail;

	string_t *tmp_guid_str;
	ARRAY_TYPE(mailbox_expunge_rec) expunges;
	unsigned int expunge_idx;
	unsigned int expunges_set:1;
};

struct local_dsync_mailbox {
	struct mail_namespace *ns;
	mailbox_guid_t guid;
	const char *storage_name;
};

struct local_dsync_worker {
	struct dsync_worker worker;
	struct mail_user *user;

	pool_t pool;
	/* mailbox_guid_t -> struct local_dsync_mailbox* */
	struct hash_table *mailbox_hash;

	mailbox_guid_t selected_box_guid;
	struct mailbox *selected_box;
	struct mail *mail;
};

extern struct dsync_worker_vfuncs local_dsync_worker;

static void worker_mailbox_close(struct local_dsync_worker *worker);

static int mailbox_guid_cmp(const void *p1, const void *p2)
{
	const mailbox_guid_t *g1 = p1, *g2 = p2;

	return memcmp(g1->guid, g2->guid, sizeof(g1->guid));
}

static unsigned int mailbox_guid_hash(const void *p)
{
	const mailbox_guid_t *guid = p;
        const uint8_t *s = guid->guid;
	unsigned int i, g, h = 0;

	for (i = 0; i < sizeof(guid->guid); i++) {
		h = (h << 4) + s[i];
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
	}
	return h;
}

struct dsync_worker *dsync_worker_init_local(struct mail_user *user)
{
	struct local_dsync_worker *worker;
	pool_t pool;

	pool = pool_alloconly_create("local dsync worker", 10240);
	worker = p_new(pool, struct local_dsync_worker, 1);
	worker->worker.v = local_dsync_worker;
	worker->user = user;
	worker->pool = pool;
	worker->mailbox_hash =
		hash_table_create(default_pool, pool, 0,
				  mailbox_guid_hash, mailbox_guid_cmp);
	return &worker->worker;
}

static void local_worker_deinit(struct dsync_worker *_worker)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	worker_mailbox_close(worker);
	hash_table_destroy(&worker->mailbox_hash);
	pool_unref(&worker->pool);
}

static bool local_worker_is_output_full(struct dsync_worker *worker ATTR_UNUSED)
{
	return FALSE;
}

static int local_worker_output_flush(struct dsync_worker *worker ATTR_UNUSED)
{
	return 1;
}

static struct dsync_worker_mailbox_iter *
local_worker_mailbox_iter_init(struct dsync_worker *_worker)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct local_dsync_worker_mailbox_iter *iter;
	enum mailbox_list_iter_flags list_flags =
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_SKIP_ALIASES |
		MAILBOX_LIST_ITER_NO_AUTO_INBOX;
	static const char *patterns[] = { "*", NULL };

	iter = i_new(struct local_dsync_worker_mailbox_iter, 1);
	iter->iter.worker = _worker;
	iter->list_iter =
		mailbox_list_iter_init_namespaces(worker->user->namespaces,
						  patterns, list_flags);
	return &iter->iter;
}

static void
local_dsync_worker_add_mailbox(struct local_dsync_worker *worker,
			       struct mail_namespace *ns,
			       const char *storage_name,
			       const mailbox_guid_t *guid)
{
	struct local_dsync_mailbox *lbox;

	lbox = p_new(worker->pool, struct local_dsync_mailbox, 1);
	lbox->ns = ns;
	memcpy(lbox->guid.guid, guid->guid, sizeof(lbox->guid.guid));
	lbox->storage_name = p_strdup(worker->pool, storage_name);

	hash_table_insert(worker->mailbox_hash, &lbox->guid, lbox);
}

static int
local_worker_mailbox_iter_next(struct dsync_worker_mailbox_iter *_iter,
			       struct dsync_mailbox *dsync_box_r)
{
	struct local_dsync_worker_mailbox_iter *iter =
		(struct local_dsync_worker_mailbox_iter *)_iter;
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_iter->worker;
	enum mailbox_flags flags =
		MAILBOX_FLAG_READONLY | MAILBOX_FLAG_KEEP_RECENT;
	const struct mailbox_info *info;
	const char *storage_name;
	struct mailbox *box;
	struct mailbox_status status;

	info = mailbox_list_iter_next(iter->list_iter);
	if (info == NULL)
		return -1;

	memset(dsync_box_r, 0, sizeof(*dsync_box_r));
	dsync_box_r->name = info->name;

	if ((info->flags & MAILBOX_NOSELECT) != 0)
		return 1;

	storage_name = mail_namespace_get_storage_name(info->ns, info->name);
	box = mailbox_alloc(info->ns->list, storage_name, NULL, flags);
	if (mailbox_sync(box, 0, 0, NULL) < 0) {
		struct mail_storage *storage = mailbox_get_storage(box);

		i_error("Failed to sync mailbox %s: %s", info->name,
			mail_storage_get_last_error(storage, NULL));
		mailbox_close(&box);
		_iter->failed = TRUE;
		return -1;
	}

	mailbox_get_status(box, STATUS_UIDNEXT | STATUS_UIDVALIDITY |
			   STATUS_HIGHESTMODSEQ | STATUS_GUID, &status);

	memcpy(dsync_box_r->guid.guid, status.mailbox_guid,
	       sizeof(dsync_box_r->guid.guid));
	dsync_box_r->uid_validity = status.uidvalidity;
	dsync_box_r->uid_next = status.uidnext;
	dsync_box_r->highest_modseq = status.highest_modseq;

	local_dsync_worker_add_mailbox(worker, info->ns, storage_name,
				       &dsync_box_r->guid);
	mailbox_close(&box);
	return 1;
}

static int
local_worker_mailbox_iter_deinit(struct dsync_worker_mailbox_iter *_iter)
{
	struct local_dsync_worker_mailbox_iter *iter =
		(struct local_dsync_worker_mailbox_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	if (mailbox_list_iter_deinit(&iter->list_iter) < 0)
		ret = -1;
	i_free(iter);
	return ret;
}

static int local_mailbox_open(struct local_dsync_worker *worker,
			      const mailbox_guid_t *guid,
			      struct mailbox **box_r)
{
	enum mailbox_flags flags = MAILBOX_FLAG_KEEP_RECENT;
	struct local_dsync_mailbox *lbox;
	struct mailbox *box;
	struct mailbox_status status;

	lbox = hash_table_lookup(worker->mailbox_hash, guid);
	if (lbox == NULL) {
		i_error("Trying to open a non-listed mailbox with guid=%s",
			binary_to_hex(guid->guid, sizeof(guid->guid)));
		return -1;
	}

	box = mailbox_alloc(lbox->ns->list, lbox->storage_name, NULL, flags);
	if (mailbox_sync(box, 0, 0, NULL) < 0) {
		struct mail_storage *storage = mailbox_get_storage(box);

		i_error("Failed to sync mailbox %s: %s", lbox->storage_name,
			mail_storage_get_last_error(storage, NULL));
		mailbox_close(&box);
		return -1;
	}
	(void)mailbox_enable(box, MAILBOX_FEATURE_CONDSTORE);

	mailbox_get_status(box, STATUS_GUID, &status);
	if (memcmp(status.mailbox_guid, guid->guid, sizeof(guid->guid)) != 0) {
		i_error("Mailbox %s changed its GUID (%s -> %s)",
			lbox->storage_name, dsync_guid_to_str(guid),
			binary_to_hex(status.mailbox_guid,
				      sizeof(status.mailbox_guid)));
		mailbox_close(&box);
		return -1;
	}
	*box_r = box;
	return 0;
}

static int iter_local_mailbox_open(struct local_dsync_worker_msg_iter *iter)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)iter->iter.worker;
	mailbox_guid_t *guid;
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mail_search_args *search_args;

	if (iter->mailbox_idx == iter->mailbox_count) {
		/* no more mailboxes */
		return -1;
	}

	guid = &iter->mailboxes[iter->mailbox_idx];
	if (local_mailbox_open(worker, guid, &box) < 0) {
		i_error("msg iteration failed: Couldn't open mailbox");
		iter->iter.failed = TRUE;
		return -1;
	}

	search_args = mail_search_build_init();
	mail_search_build_add_all(search_args);

	trans = mailbox_transaction_begin(box, 0);
	iter->search_ctx = mailbox_search_init(trans, search_args, NULL);
	iter->mail = mail_alloc(trans, MAIL_FETCH_FLAGS | MAIL_FETCH_GUID,
				NULL);
	return 0;
}

static void
iter_local_mailbox_close(struct local_dsync_worker_msg_iter *iter)
{
	struct mailbox *box = iter->mail->box;
	struct mailbox_transaction_context *trans = iter->mail->transaction;

	iter->expunges_set = FALSE;
	mail_free(&iter->mail);
	if (mailbox_search_deinit(&iter->search_ctx) < 0) {
		struct mail_storage *storage =
			mailbox_get_storage(iter->mail->box);

		i_error("msg search failed: %s",
			mail_storage_get_last_error(storage, NULL));
		iter->iter.failed = TRUE;
	}
	(void)mailbox_transaction_commit(&trans);
	mailbox_close(&box);
}

static struct dsync_worker_msg_iter *
local_worker_msg_iter_init(struct dsync_worker *worker,
			   const mailbox_guid_t mailboxes[],
			   unsigned int mailbox_count)
{
	struct local_dsync_worker_msg_iter *iter;
	unsigned int i;

	iter = i_new(struct local_dsync_worker_msg_iter, 1);
	iter->iter.worker = worker;
	iter->mailboxes = i_new(mailbox_guid_t, mailbox_count);
	iter->mailbox_count = mailbox_count;
	for (i = 0; i < mailbox_count; i++) {
		memcpy(iter->mailboxes[i].guid, &mailboxes[i],
		       sizeof(iter->mailboxes[i].guid));
	}
	i_array_init(&iter->expunges, 32);
	iter->tmp_guid_str = str_new(default_pool, MAIL_GUID_128_SIZE * 2 + 1);
	(void)iter_local_mailbox_open(iter);
	return &iter->iter;
}

static bool
iter_local_mailbox_next_expunge(struct local_dsync_worker_msg_iter *iter,
				uint32_t prev_uid, struct dsync_message *msg_r)
{
	struct mailbox *box = iter->mail->box;
	struct mailbox_status status;
	const struct mailbox_expunge_rec *expunges;
	unsigned int count;

	if (iter->expunges_set) {
		expunges = array_get(&iter->expunges, &count);
		if (iter->expunge_idx == count)
			return FALSE;

		memset(msg_r, 0, sizeof(*msg_r));
		str_truncate(iter->tmp_guid_str, 0);
		binary_to_hex_append(iter->tmp_guid_str,
				     expunges[iter->expunge_idx].guid_128,
				     MAIL_GUID_128_SIZE);
		msg_r->guid = str_c(iter->tmp_guid_str);
		msg_r->uid = expunges[iter->expunge_idx].uid;
		iter->expunge_idx++;
		return TRUE;
	}

	iter->expunge_idx = 0;
	array_clear(&iter->expunges);
	iter->expunges_set = TRUE;

	mailbox_get_status(box, STATUS_UIDNEXT, &status);
	if (prev_uid + 1 >= status.uidnext) {
		/* no expunged messages at the end of mailbox */
		return FALSE;
	}

	T_BEGIN {
		ARRAY_TYPE(seq_range) uids_filter;

		t_array_init(&uids_filter, 1);
		seq_range_array_add_range(&uids_filter, prev_uid + 1,
					  status.uidnext - 1);
		(void)mailbox_get_expunges(box, 0, &uids_filter,
					   &iter->expunges);
	} T_END;
	return iter_local_mailbox_next_expunge(iter, prev_uid, msg_r);
}

static int
local_worker_msg_iter_next(struct dsync_worker_msg_iter *_iter,
			   unsigned int *mailbox_idx_r,
			   struct dsync_message *msg_r)
{
	struct local_dsync_worker_msg_iter *iter =
		(struct local_dsync_worker_msg_iter *)_iter;
	const char *guid;
	uint32_t prev_uid;

	if (_iter->failed || iter->search_ctx == NULL)
		return -1;

	prev_uid = iter->mail->uid;
	switch (mailbox_search_next(iter->search_ctx, iter->mail)) {
	case 0:
		if (iter_local_mailbox_next_expunge(iter, prev_uid, msg_r)) {
			*mailbox_idx_r = iter->mailbox_idx;
			return 1;
		}
		iter_local_mailbox_close(iter);
		iter->mailbox_idx++;
		if (iter_local_mailbox_open(iter) < 0)
			return -1;
		return local_worker_msg_iter_next(_iter, mailbox_idx_r, msg_r);
	case -1:
		return -1;
	default:
		break;
	}
	*mailbox_idx_r = iter->mailbox_idx;

	if (mail_get_special(iter->mail, MAIL_FETCH_GUID, &guid) < 0) {
		if (!iter->mail->expunged) {
			struct mail_storage *storage =
				mailbox_get_storage(iter->mail->box);

			i_error("msg guid lookup failed: %s",
				mail_storage_get_last_error(storage, NULL));
			_iter->failed = TRUE;
			return -1;
		}
		return local_worker_msg_iter_next(_iter, mailbox_idx_r, msg_r);
	}

	memset(msg_r, 0, sizeof(*msg_r));
	msg_r->guid = guid;
	msg_r->uid = iter->mail->uid;
	msg_r->flags = mail_get_flags(iter->mail);
	msg_r->keywords = mail_get_keywords(iter->mail);
	msg_r->modseq = mail_get_modseq(iter->mail);
	return 1;
}

static int
local_worker_msg_iter_deinit(struct dsync_worker_msg_iter *_iter)
{
	struct local_dsync_worker_msg_iter *iter =
		(struct local_dsync_worker_msg_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	if (iter->mail != NULL)
		iter_local_mailbox_close(iter);
	array_free(&iter->expunges);
	str_free(&iter->tmp_guid_str);
	i_free(iter->mailboxes);
	i_free(iter);
	return ret;
}

static void
local_worker_copy_mailbox_update(const struct dsync_mailbox *dsync_box,
				 struct mailbox_update *update_r)
{
	memset(update_r, 0, sizeof(*update_r));
	memcpy(update_r->mailbox_guid, dsync_box->guid.guid,
	       sizeof(update_r->mailbox_guid));
	update_r->uid_validity = dsync_box->uid_validity;
	update_r->min_next_uid = dsync_box->uid_next;
	update_r->min_highest_modseq = dsync_box->highest_modseq;
}

static struct mailbox *
local_worker_mailbox_alloc(struct local_dsync_worker *worker,
			   const struct dsync_mailbox *dsync_box)
{
	struct mail_namespace *ns;
	const char *name;

	name = dsync_box->name;
	ns = mail_namespace_find(worker->user->namespaces, &name);
	if (ns == NULL) {
		i_error("Can't find namespace for mailbox %s", dsync_box->name);
		return NULL;
	}

	return mailbox_alloc(ns->list, name, NULL, 0);
}

static void
local_worker_create_mailbox(struct dsync_worker *_worker,
			    const struct dsync_mailbox *dsync_box)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox *box;
	struct mailbox_update update;
	int ret;

	box = local_worker_mailbox_alloc(worker, dsync_box);
	if (box == NULL) {
		dsync_worker_set_failure(_worker);
		return;
	}
	local_worker_copy_mailbox_update(dsync_box, &update);

	if (strcasecmp(dsync_box->name, "INBOX") == 0)
		ret = mailbox_update(box, &update);
	else {
		ret = mailbox_create(box, &update,
				     dsync_box->uid_validity == 0);
	}
	if (ret < 0) {
		dsync_worker_set_failure(_worker);
		i_error("Can't create mailbox %s: %s", dsync_box->name,
			mail_storage_get_last_error(mailbox_get_storage(box),
						    NULL));
	} else {
		local_dsync_worker_add_mailbox(worker,
					       mailbox_get_namespace(box),
					       mailbox_get_name(box),
					       &dsync_box->guid);
	}
	mailbox_close(&box);
}

static void worker_mailbox_close(struct local_dsync_worker *worker)
{
	struct mailbox_transaction_context *trans;

	if (worker->selected_box != NULL) {
		trans = worker->mail->transaction;
		mail_free(&worker->mail);
		if (mailbox_transaction_commit(&trans) < 0)
			dsync_worker_set_failure(&worker->worker);
		mailbox_close(&worker->selected_box);
	}
}

static void
local_worker_update_mailbox(struct dsync_worker *_worker,
			    const struct dsync_mailbox *dsync_box)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox *box;
	struct mailbox_update update;

	if (worker->selected_box != NULL &&
	    dsync_guid_equals(&dsync_box->guid, &worker->selected_box_guid))
		worker_mailbox_close(worker);

	box = local_worker_mailbox_alloc(worker, dsync_box);
	if (box == NULL) {
		dsync_worker_set_failure(_worker);
		return;
	}

	local_worker_copy_mailbox_update(dsync_box, &update);
	if (mailbox_update(box, &update) < 0) {
		dsync_worker_set_failure(_worker);
		i_error("Can't update mailbox %s: %s", dsync_box->name,
			mail_storage_get_last_error(mailbox_get_storage(box),
						    NULL));
	}
	mailbox_close(&box);
}

static void
local_worker_select_mailbox(struct dsync_worker *_worker,
			    const mailbox_guid_t *mailbox)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox_transaction_context *trans;

	if (worker->selected_box != NULL) {
		if (dsync_guid_equals(&worker->selected_box_guid, mailbox))
			return;
		worker_mailbox_close(worker);
	}
	worker->selected_box_guid = *mailbox;

	if (local_mailbox_open(worker, mailbox, &worker->selected_box) < 0)
		dsync_worker_set_failure(_worker);
	else {
		trans = mailbox_transaction_begin(worker->selected_box,
					MAILBOX_TRANSACTION_FLAG_EXTERNAL |
					MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS);
		worker->mail = mail_alloc(trans, 0, NULL);
	}
}

static void
local_worker_msg_update_metadata(struct dsync_worker *_worker,
				 const struct dsync_message *msg)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mail_keywords *keywords;

	if (!mail_set_uid(worker->mail, msg->uid))
		dsync_worker_set_failure(_worker);
	else {
		mail_update_flags(worker->mail, MODIFY_REPLACE, msg->flags);

		keywords = mailbox_keywords_create_valid(worker->mail->box,
							 msg->keywords);
		mail_update_keywords(worker->mail, MODIFY_REPLACE, keywords);
		mailbox_keywords_unref(worker->mail->box, &keywords);
		mail_update_modseq(worker->mail, msg->modseq);
	}
}

static void
local_worker_msg_update_uid(struct dsync_worker *_worker,
			    uint32_t old_uid, uint32_t new_uid)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	if (!mail_set_uid(worker->mail, old_uid))
		dsync_worker_set_failure(_worker);
	else
		mail_update_uid(worker->mail, new_uid);
}

static void local_worker_msg_expunge(struct dsync_worker *_worker, uint32_t uid)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;

	if (mail_set_uid(worker->mail, uid))
		mail_expunge(worker->mail);
}

static void
local_worker_msg_save_set_metadata(struct mailbox *box,
				   struct mail_save_context *save_ctx,
				   const struct dsync_message *msg)
{
	struct mail_keywords *keywords;

	keywords = str_array_length(msg->keywords) == 0 ? NULL :
		mailbox_keywords_create_valid(box, msg->keywords);
	mailbox_save_set_flags(save_ctx, msg->flags, keywords);
	if (keywords != NULL)
		mailbox_keywords_unref(box, &keywords);
	mailbox_save_set_uid(save_ctx, msg->uid);
	mailbox_save_set_save_date(save_ctx, msg->save_date);
	mailbox_save_set_min_modseq(save_ctx, msg->modseq);
}

static void
local_worker_msg_copy(struct dsync_worker *_worker,
		      const mailbox_guid_t *src_mailbox, uint32_t src_uid,
		      const struct dsync_message *dest_msg,
		      dsync_worker_copy_callback_t *callback, void *context)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mailbox *src_box;
	struct mailbox_transaction_context *src_trans;
	struct mail *src_mail;
	struct mail_save_context *save_ctx;
	int ret;

	if (local_mailbox_open(worker, src_mailbox, &src_box) < 0) {
		callback(FALSE, context);
		return;
	}

	src_trans = mailbox_transaction_begin(src_box, 0);
	src_mail = mail_alloc(src_trans, 0, NULL);
	if (!mail_set_uid(src_mail, src_uid))
		ret = -1;
	else {
		save_ctx = mailbox_save_alloc(worker->mail->transaction);
		local_worker_msg_save_set_metadata(worker->mail->box,
						   save_ctx, dest_msg);
		ret = mailbox_copy(&save_ctx, src_mail);
	}

	mail_free(&src_mail);
	(void)mailbox_transaction_commit(&src_trans);
	mailbox_close(&src_box);

	callback(ret == 0, context);
}

static void
local_worker_save_msg_continue(struct local_dsync_worker *worker,
			       struct mail_save_context *save_ctx,
			       struct istream *input)
{
	int ret;

	while ((ret = i_stream_read(input)) > 0) {
		if (mailbox_save_continue(save_ctx) < 0)
			break;
	}
	i_assert(ret == -1);

	if (input->stream_errno != 0) {
		errno = input->stream_errno;
		i_error("read(msg input) failed: %m");
		mailbox_save_cancel(&save_ctx);
		ret = -1;
	} else {
		i_assert(input->eof);
		ret = mailbox_save_finish(&save_ctx);
	}
	if (ret < 0)
		dsync_worker_set_failure(&worker->worker);
}

static void
local_worker_msg_save(struct dsync_worker *_worker,
		      const struct dsync_message *msg,
		      const struct dsync_msg_static_data *data)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct mail_save_context *save_ctx;

	save_ctx = mailbox_save_alloc(worker->mail->transaction);
	mailbox_save_set_guid(save_ctx, msg->guid);
	local_worker_msg_save_set_metadata(worker->mail->box, save_ctx, msg);
	mailbox_save_set_pop3_uidl(save_ctx, data->pop3_uidl);

	mailbox_save_set_received_date(save_ctx, data->received_date, 0);

	if (mailbox_save_begin(&save_ctx, data->input) < 0) {
		dsync_worker_set_failure(_worker);
		return;
	}
	local_worker_save_msg_continue(worker, save_ctx, data->input);
}

static void
local_worker_msg_get(struct dsync_worker *_worker, uint32_t uid,
		     dsync_worker_msg_callback_t *callback, void *context)
{
	struct local_dsync_worker *worker =
		(struct local_dsync_worker *)_worker;
	struct dsync_msg_static_data data;

	if (worker->mail == NULL) {
		/* no mailbox is selected */
		callback(DSYNC_MSG_GET_RESULT_FAILED, NULL, context);
		return;
	}

	if (!mail_set_uid(worker->mail, uid)) {
		callback(DSYNC_MSG_GET_RESULT_EXPUNGED, NULL, context);
		return;
	}

	memset(&data, 0, sizeof(data));
	if (mail_get_special(worker->mail, MAIL_FETCH_UIDL_BACKEND,
			     &data.pop3_uidl) < 0 ||
	    mail_get_received_date(worker->mail, &data.received_date) < 0 ||
	    mail_get_stream(worker->mail, NULL, NULL, &data.input) < 0) {
		if (worker->mail->expunged)
			callback(DSYNC_MSG_GET_RESULT_EXPUNGED, NULL, context);
		else
			callback(DSYNC_MSG_GET_RESULT_FAILED, NULL, context);
	} else {
		callback(DSYNC_MSG_GET_RESULT_SUCCESS, &data, context);
	}
}

struct dsync_worker_vfuncs local_dsync_worker = {
	local_worker_deinit,

	local_worker_is_output_full,
	local_worker_output_flush,

	local_worker_mailbox_iter_init,
	local_worker_mailbox_iter_next,
	local_worker_mailbox_iter_deinit,

	local_worker_msg_iter_init,
	local_worker_msg_iter_next,
	local_worker_msg_iter_deinit,

	local_worker_create_mailbox,
	local_worker_update_mailbox,

	local_worker_select_mailbox,
	local_worker_msg_update_metadata,
	local_worker_msg_update_uid,
	local_worker_msg_expunge,
	local_worker_msg_copy,
	local_worker_msg_save,
	local_worker_msg_get
};
