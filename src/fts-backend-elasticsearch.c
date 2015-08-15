#include "lib.h"
#include "array.h"
#include "str.h"
#include "hash.h"
#include "strescape.h"
#include "unichar.h"
#include "mail-storage-private.h"
#include "mailbox-list-private.h"
#include "mail-search.h"
#include "fts-api.h"
#include "fts-elasticsearch-plugin.h"
#include "elasticsearch-conn.h"

#include <ctype.h>
#include <syslog.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>
#include <json-c/json.h>

#define ELASTICSEARCH_BULK_SIZE 15000000 /* 15 megabytes */

struct elasticsearch_fts_backend {
    struct fts_backend backend;
    struct elasticsearch_connection *elasticsearch_conn;
};

struct elasticsearch_fts_backend_update_context {
    struct fts_backend_update_context ctx;

    struct mailbox *prev_box;
    char box_guid[MAILBOX_GUID_HEX_LENGTH + 1];
    
    uint32_t prev_uid;

    /* used to build multi-part messages. */
    string_t *temp;
    string_t *current_field;

    /* we store this as a string due to the way ES handles bulk indexing JSON.
     * it is not actually valid JSON and thus can't be built with json-c. */
    string_t *json_request;

    /* expunges and updates get called in the same message */
    string_t *expunge_json_request;

    /* current request size */
    size_t request_size;

    /* builds the current message as a JSON object so we can append it later. */
    json_object *message;

    unsigned int body_open:1;
    unsigned int documents_added:1;
    unsigned int expunges:1;
    unsigned int truncate_header:1;
};

static struct fts_backend *fts_backend_elasticsearch_alloc(void)
{
    struct elasticsearch_fts_backend *backend;

    backend = i_new(struct elasticsearch_fts_backend, 1);
    backend->backend = fts_backend_elasticsearch;

    return &backend->backend;
}

static int
fts_backend_elasticsearch_init(struct fts_backend *_backend,
                               const char **error_r ATTR_UNUSED)
{
    struct elasticsearch_fts_backend *backend = NULL;
    struct fts_elasticsearch_user *fuser = NULL;

    /* ensure our backend is provided */
    if (_backend != NULL) {
        backend = (struct elasticsearch_fts_backend *)_backend;
    } else {
        *error_r = "fts_elasticsearch: error during backend initilisation";

        return -1;
    }
    
    fuser = FTS_ELASTICSEARCH_USER_CONTEXT(_backend->ns->user);

    if (fuser == NULL) {
        *error_r = "Invalid fts_elasticsearch setting";

        return -1;
    }

    return elasticsearch_connection_init(fuser->set.url, fuser->set.debug,
                                         &backend->elasticsearch_conn, error_r);
}

static void
fts_backend_elasticsearch_deinit(struct fts_backend *_backend)
{
    i_free(_backend);
}

static void
fts_backend_elasticsearch_doc_close(struct elasticsearch_fts_backend_update_context *_ctx)
{
    struct elasticsearch_fts_backend_update_context *ctx = NULL;

    /* ensure we have a context */
    if (_ctx != NULL) {
        ctx = (struct elasticsearch_fts_backend_update_context *)_ctx;

        /* convert our completed message to a string and tack it on to our request */
        str_append(ctx->json_request, json_object_to_json_string(ctx->message));
        str_append(ctx->json_request, "\n");

        /* clean up our json object, it is no longer required */
        json_object_put(ctx->message);

        /* clean-up for the next message */
        str_truncate(ctx->temp, 0);
        str_truncate(ctx->current_field, 0);

        if (ctx->body_open) {
            ctx->body_open = FALSE;
        } else {
            /* nothing to do if the body isn't open */
        }
    }
}

static int
fts_backend_elasticsearch_get_last_uid(struct fts_backend *_backend,
                                       struct mailbox *box,
                                       uint32_t *last_uid_r)
{
    struct fts_index_header hdr;
    struct elasticsearch_fts_backend *backend = NULL;
    const char *box_guid = NULL;
    json_object *root = NULL, *sort_root = NULL;
    json_object *query_root = NULL, *fields_root = NULL;
    int32_t ret;

    /* ensure our backend has been initialised */
    if (_backend == NULL || box == NULL || last_uid_r == NULL) {
        i_error("fts_elasticsearch: critical error in get_last_uid");

        return -1;
    } else {
        /* keep track of our backend */
        backend = (struct elasticsearch_fts_backend *)_backend;
    }

    /**
     * assume the dovecot index will always match ours for uids. this saves
     * on repeated calls to ES when fts_autoindex=true.
     *
     * this has a couple of side effects:
     *  1. if the ES index has been blown away, this will return a valid
     *     last_uid that matches Dovecot and it won't realise we need updating
     *  2. if data has been indexed by Dovecot but missed by ES (outage, etc)
     *     then it won't ever make it to the ES index either.
     *
     * TODO: find a better way to implement this
     **/
    if (fts_index_get_header(box, &hdr)) {
        *last_uid_r = hdr.last_indexed_uid;

        return 0;
    }

    if (fts_mailbox_get_guid(box, &box_guid) < 0) {
        i_error("fts-elasticsearch: get_last_uid: failed to get mbox guid");

        return -1;
    }

    /* build a JSON object to query the last uid */
    root = json_object_new_object();
    sort_root = json_object_new_object();
    query_root = json_object_new_object();
    fields_root = json_object_new_array();

    json_object_object_add(sort_root, "uid", json_object_new_string("desc"));
    json_object_object_add(query_root, "match_all", json_object_new_object());
    json_object_array_add(fields_root, json_object_new_string("uid"));

    json_object_object_add(root, "sort", sort_root);
    json_object_object_add(root, "query", query_root);
    json_object_object_add(root, "fields", fields_root);
    json_object_object_add(root, "size", json_object_new_int(1));

    /* call ES */
    ret = elasticsearch_connection_last_uid(backend->elasticsearch_conn,
        json_object_to_json_string(root), box_guid);

    /* clean it up */
    json_object_put(root);

    if (ret > 0) {
        *last_uid_r = ret;

        fts_index_set_last_uid(box, *last_uid_r);

        return 0;
    }
    
    *last_uid_r = 0;

    fts_index_set_last_uid(box, *last_uid_r);

    return 0;
}

static struct fts_backend_update_context *
fts_backend_elasticsearch_update_init(struct fts_backend *_backend)
{
    struct elasticsearch_fts_backend_update_context *ctx = NULL;

    ctx = i_new(struct elasticsearch_fts_backend_update_context, 1);
    ctx->ctx.backend = _backend;
    ctx->current_field = NULL;
    ctx->temp = NULL;
    ctx->json_request = NULL;
    ctx->expunge_json_request = NULL;

    return &ctx->ctx;
}

static int
fts_backend_elasticsearch_update_deinit(struct fts_backend_update_context *_ctx)
{
    struct elasticsearch_fts_backend_update_context *ctx = NULL;
    struct elasticsearch_fts_backend *backend = NULL;

    /* validate our input parameters */
    if (_ctx == NULL || _ctx->backend == NULL) {
        i_error("fts_elasticsearch: critical error in update_deinit");
        return -1;
    } else {
        ctx = (struct elasticsearch_fts_backend_update_context *)_ctx;
        backend = (struct elasticsearch_fts_backend *)_ctx->backend;
    }

    /* expunges will also end up here; only clean-up and post updates */
    if (ctx->json_request != NULL) {
        /* this gets called when the last message is finished, so close it up */
        fts_backend_elasticsearch_doc_close(ctx);

        /* do our bulk post */
        elasticsearch_connection_update(backend->elasticsearch_conn,
                                        str_c(ctx->json_request));

        /* cleanup */
        memset(ctx->box_guid, 0, sizeof(ctx->box_guid));
        str_free(&ctx->current_field);
        str_free(&ctx->temp);
        str_free(&ctx->json_request); 
        ctx->request_size = 0;
    }

    /* if there have been any expunges, we should process them now */
    if (ctx->expunges) {
        /* do our bulk post */
        elasticsearch_connection_update(backend->elasticsearch_conn,
                                        str_c(ctx->expunge_json_request));

        str_free(&ctx->expunge_json_request);
    }

    i_free(ctx);
    
    return 0;
}

static void
fts_backend_elasticsearch_update_set_mailbox(struct fts_backend_update_context *_ctx,
                                             struct mailbox *box)
{
    struct elasticsearch_fts_backend_update_context *ctx = NULL;
    const char *box_guid = NULL;

    if (_ctx != NULL) {
        ctx = (struct elasticsearch_fts_backend_update_context *)_ctx;

        /* update_set_mailbox has been called but the previous uid is not 0;
         * clean up from our previous mailbox indexing. */
        if (ctx->prev_uid != 0) {
            fts_index_set_last_uid(ctx->prev_box, ctx->prev_uid);
            ctx->prev_uid = 0;
        }

        if (box != NULL) {
            if (fts_mailbox_get_guid(box, &box_guid) < 0) {
                i_debug("fts-elasticsearch: update_set_mailbox: fts_mailbox_get_guid failed");
                _ctx->failed = TRUE;
            } else {
                /* successfuly got mailbox GUID and can continue */
            }

            /* store the current mailbox we're on in our state struct */
            i_assert(strlen(box_guid) == sizeof(ctx->box_guid) - 1);
            memcpy(ctx->box_guid, box_guid, sizeof(ctx->box_guid) - 1);
        } else {
            /* a box of null appears to indicate that indexing is complete. */
        }

        ctx->prev_box = box;
    } else {
        i_error("fts_elasticsearch: update_set_mailbox: context was NULL");
    }
}

static void
fts_backend_elasticsearch_doc_open(struct elasticsearch_fts_backend_update_context *_ctx,
                                   uint32_t uid, string_t *json_request,
                                   json_object *message, const char *action_name)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;
    json_object *temp = NULL, *action = NULL, *jint = NULL, *jstring = NULL;

    /* track that we've added documents */
    ctx->documents_added = TRUE;

    /* TODO: this json-c code must leak like crazy? i'm not sure how it handles
     * reference counts. */

    temp = json_object_new_object();

    json_object_object_add(temp, "_index", json_object_new_string(ctx->box_guid));
    json_object_object_add(temp, "_type", json_object_new_string("mail"));
    json_object_object_add(temp, "_id", json_object_new_int(uid));

    action = json_object_new_object();
    json_object_object_add(action, action_name, temp);

    str_append(json_request, json_object_to_json_string(action));
    str_append(json_request, "\n");

    jint = json_object_new_int(uid);
    json_object_object_add(message, "uid", jint);

    jstring = json_object_new_string(ctx->box_guid);
    json_object_object_add(message, "box", jstring);

    /* clean-up */
    json_object_put(action);
}

static void
fts_backend_elasticsearch_uid_changed(struct fts_backend_update_context *_ctx,
                                      uint32_t uid)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;
    struct elasticsearch_fts_backend *backend =
            (struct elasticsearch_fts_backend *)_ctx->backend;

    if (!ctx->documents_added) {
        i_assert(ctx->prev_uid == 0);

        /* allocate strings for building messages and multi-part messages
         * with a sensible initial size. */
        ctx->current_field = str_new(default_pool, 1024 * 64);
        ctx->temp = str_new(default_pool, 1024 * 64);
        ctx->json_request = str_new(default_pool, 1024 * 64);
        ctx->request_size = 0;
    } else {
        /* this is the end of an old message. nb: the last message to be indexed
         * will not reach here but will instead be caught in update_deinit. */
        fts_backend_elasticsearch_doc_close(ctx);
    }

    /* chunk up our requests in to reasonable sizes */
    if (ctx->request_size > ELASTICSEARCH_BULK_SIZE) {        
        /* do an early post */
        elasticsearch_connection_update(backend->elasticsearch_conn,
                                        str_c(ctx->json_request));

        /* reset our tracking variables */
        str_truncate(ctx->json_request, 0);
        ctx->request_size = 0;
    }
    
    ctx->prev_uid = uid;
    ctx->truncate_header = FALSE;
    ctx->message = json_object_new_object();
    fts_backend_elasticsearch_doc_open(ctx, uid, ctx->json_request,
                                       ctx->message, "index");
}

static bool
fts_backend_elasticsearch_update_set_build_key(struct fts_backend_update_context *_ctx,
                                         const struct fts_backend_build_key *key)
{
    struct elasticsearch_fts_backend_update_context *ctx = NULL;

    /* validate our input */
    if (_ctx == NULL || key == NULL) {
        return FALSE;
    } else {
        ctx = (struct elasticsearch_fts_backend_update_context *)_ctx;
    }

    /* if the uid doesn't match our expected one, we've moved on to a new message */
    if (key->uid != ctx->prev_uid)
        fts_backend_elasticsearch_uid_changed(_ctx, key->uid);

    switch (key->type) {
    case FTS_BACKEND_BUILD_KEY_HDR: /* fall through */
    case FTS_BACKEND_BUILD_KEY_MIME_HDR:
        str_printfa(ctx->current_field, "%s", t_str_lcase(key->hdr_name));

        break;
    case FTS_BACKEND_BUILD_KEY_BODY_PART:
        if (!ctx->body_open) {
            ctx->body_open = TRUE;
            str_append(ctx->current_field, "body");
        }

        break;
    case FTS_BACKEND_BUILD_KEY_BODY_PART_BINARY:
        i_unreached();
    }

    return TRUE;
}

static int
fts_backend_elasticsearch_update_build_more(struct fts_backend_update_context *_ctx,
                                      const unsigned char *data, size_t size)
{
    struct elasticsearch_fts_backend_update_context *ctx;

    if (_ctx != NULL) {
        ctx = (struct elasticsearch_fts_backend_update_context *)_ctx;

        /* build more message body */
        str_append_n(ctx->temp, data, size);

        /* keep track of the total request size for chunking */
        ctx->request_size += size;

        return 0;
    } else {
        i_error("fts_elasticsearch: update_build_more: critical error building message body");

        return -1;
    }
}

static void
fts_backend_elasticsearch_update_unset_build_key(struct fts_backend_update_context *_ctx)
{
    struct elasticsearch_fts_backend_update_context *ctx = NULL;
    json_object *jstring = NULL;

    if (_ctx != NULL) {
        ctx = (struct elasticsearch_fts_backend_update_context *)_ctx;

        /* field is complete, add it to our message. */
        jstring = json_object_new_string(str_c(ctx->temp));
        json_object_object_add(ctx->message, str_c(ctx->current_field), jstring);

        /* clean-up our temp */
        str_truncate(ctx->temp, 0);
        str_truncate(ctx->current_field, 0);
    }
}

static void
fts_backend_elasticsearch_update_expunge(struct fts_backend_update_context *_ctx,
                                         uint32_t uid)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;
    json_object *message = NULL;

    /* update the context to note that there have been expunges */
    ctx->expunges = TRUE;

    /* set-up our json request */
    if (ctx->expunge_json_request == NULL) {
        ctx->expunge_json_request = str_new(default_pool, 1024 * 64);
    } else {
        /* ctx->expunge_json_request was allocated in an earlier call */
    }

    message = json_object_new_object();

    /* we don't need a corresponding doc_close call, the bulk delete API is shorter */
    fts_backend_elasticsearch_doc_open(ctx, uid, ctx->expunge_json_request,
                                       message, "delete");

    /* clean-up */
    json_object_put(message);
}

static int fts_backend_elasticsearch_refresh(struct fts_backend *_backend)
{
    struct elasticsearch_fts_backend *backend =
        (struct elasticsearch_fts_backend *)_backend;

    elasticsearch_connection_refresh(backend->elasticsearch_conn);

    return 0;
}

static int fts_backend_elasticsearch_rescan(struct fts_backend *backend ATTR_UNUSED)
{    
    return fts_backend_reset_last_uids(backend);
}

static int fts_backend_elasticsearch_optimize(struct fts_backend *backend ATTR_UNUSED)
{
    return 0;
}

static bool
elasticsearch_add_definite_query(struct mail_search_arg *arg, json_object *value,
                                 json_object *fields)
{
    /* validate our input */
    if (arg == NULL || value == NULL || fields == NULL) {
        i_error("fts_elasticsearch: critical error while building query");

        return FALSE;
    }

    switch (arg->type) {
    case SEARCH_TEXT:
        /* we don't actually have to do anything here; leaving the fields
         * array blank is sufficient to cause full text search with ES */

        break;
    case SEARCH_BODY:
        /* SEARCH_BODY has a hdr_field_name of null. */
        json_object_array_add(fields, json_object_new_string("body"));

        break;
    case SEARCH_HEADER: /* fall through */
    case SEARCH_HEADER_ADDRESS: /* fall through */
    case SEARCH_HEADER_COMPRESS_LWSP:
        if (!fts_header_want_indexed(arg->hdr_field_name)) {
            i_debug("fts-elasticsearch: field %s was skipped", arg->hdr_field_name);

            return FALSE;
        }

        json_object_array_add(fields,
            json_object_new_string(t_str_lcase(arg->hdr_field_name)));

        break;
    default:
        return FALSE;
    }

    /* TODO: can we wrap a query_string in a not filter? */
    if (arg->match_not) {
        i_debug("fts-elasticsearch: arg->match_not is true");
    }

    /* we always want to add a query value */
    json_object_object_add(value, "query", json_object_new_string(arg->value.str));

    return TRUE;
}

static bool
elasticsearch_add_definite_query_args(json_object *fields, json_object *value,
                                      struct mail_search_arg *arg)
{
    bool field_added = FALSE;

    if (fields == NULL || value == NULL || arg == NULL) {
        i_error("fts_elasticsearch: critical error while building query");

        return FALSE;
    }

    for (; arg != NULL; arg = arg->next) {
        /* multiple fields have an initial arg of nothing useful and subargs */
        if (arg->value.subargs != NULL) {
            field_added = elasticsearch_add_definite_query_args(fields, value,
                arg->value.subargs);
        } else {
            /* no subargs to process */
        }

        if (elasticsearch_add_definite_query(arg, value, fields)) {
            /* this is important to set. if this is FALSE, Dovecot will fail
             * over to its regular built-in search to produce results for
             * this argument. */
            arg->match_always = TRUE;
            field_added = TRUE;
        }
    }

    return field_added;
}

static int
fts_backend_elasticsearch_lookup(struct fts_backend *_backend, struct mailbox *box,
                                 struct mail_search_arg *args,
                                 enum fts_lookup_flags flags,
                                 struct fts_result *result)
{
    struct elasticsearch_fts_backend *backend = NULL;
    struct elasticsearch_result **es_results = NULL;
    struct mailbox_status status;
    json_object *term = NULL, *fields = NULL, *value = NULL;
    json_object *query = NULL, *fields_root = NULL;
    const char *box_guid = NULL;
    bool valid = FALSE;
    bool and_args = (flags & FTS_LOOKUP_FLAG_AND_ARGS) != 0;
    pool_t pool;
    int32_t ret = -1;

    /* validate our input */
    if (_backend == NULL || box == NULL || args == NULL || result == NULL)
    {
        i_error("fts_elasticsearch: critical error during lookup");

        return -1;
    } else {
        /* safe to continue */
    }

    backend = (struct elasticsearch_fts_backend *)_backend;
    pool = pool_alloconly_create("fts elasticsearch search", 1024);

    /* get the mailbox guid */
    if (fts_mailbox_get_guid(box, &box_guid) < 0) {
        return -1;
    }

    /* open the mailbox */
    mailbox_get_open_status(box, STATUS_UIDNEXT, &status);

    /* TODO: pagination, status.uidnext shows where we're up to. */

    /* start building our query object */
    term = json_object_new_object();
    fields = json_object_new_array();
    value = json_object_new_object();

    /* build the query */
    valid = elasticsearch_add_definite_query_args(fields, value, args);

    /* return early if it failed */
    if (!valid) {
        return -1;
    }

    /* if no fields were added, add _all as our only field */
    if (json_object_array_length(fields) == 0) {
        json_object_array_add(fields, json_object_new_string("_all"));
    }

    /* determine if we should AND or OR */
    if (and_args) {
        json_object_object_add(value, "operator", json_object_new_string("and"));
    }
    else {
        json_object_object_add(value, "operator", json_object_new_string("or"));
    }
    
    json_object_object_add(value, "fields", fields);
    json_object_object_add(term, "multi_match", value);

    /* wrap it in the ES 'query' field */
    query = json_object_new_object();
    json_object_object_add(query, "query", term);

    /* only return the UID field */
    fields_root = json_object_new_array();
    json_object_array_add(fields_root, json_object_new_string("uid"));
    json_object_array_add(fields_root, json_object_new_string("box"));
    json_object_object_add(query, "fields", fields_root);
    json_object_object_add(query, "size", json_object_new_int(INT_MAX));
    
    ret = elasticsearch_connection_select(backend->elasticsearch_conn, pool,
        json_object_to_json_string(query), box_guid, &es_results);

    /* build our fts_result return */
    result->box = box;
    result->scores_sorted = FALSE;

    /* FTS_LOOKUP_FLAG_NO_AUTO_FUZZY says that exact matches for non-fuzzy searches
     * should go to maybe_uids instead of definite_uids. */
    ARRAY_TYPE(seq_range) *uids_arr = (flags & FTS_LOOKUP_FLAG_NO_AUTO_FUZZY) == 0 ?
            &result->definite_uids : &result->maybe_uids;

    if (ret > 0 && es_results != NULL) {
        array_append_array(uids_arr, &es_results[0]->uids);
        array_append_array(&result->scores, &es_results[0]->scores);
    }

    /* clean-up */
    json_object_put(query);
    pool_unref(&pool);

    return ret;
}

struct fts_backend fts_backend_elasticsearch = {
    .name = "elasticsearch",
    .flags = FTS_BACKEND_FLAG_FUZZY_SEARCH,

    {
        fts_backend_elasticsearch_alloc,
        fts_backend_elasticsearch_init,
        fts_backend_elasticsearch_deinit,
        fts_backend_elasticsearch_get_last_uid,
        fts_backend_elasticsearch_update_init,
        fts_backend_elasticsearch_update_deinit,
        fts_backend_elasticsearch_update_set_mailbox,
        fts_backend_elasticsearch_update_expunge,
        fts_backend_elasticsearch_update_set_build_key,
        fts_backend_elasticsearch_update_unset_build_key,
        fts_backend_elasticsearch_update_build_more,
        fts_backend_elasticsearch_refresh,
        fts_backend_elasticsearch_rescan,
        fts_backend_elasticsearch_optimize,
        fts_backend_default_can_lookup,
        fts_backend_elasticsearch_lookup,
        NULL
    }
};
