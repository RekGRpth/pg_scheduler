#include "include.h"

extern char *default_null;
static void work_query(Task *task);

static bool work_is_log_level_output(int elevel, int log_min_level) {
    if (elevel == LOG || elevel == LOG_SERVER_ONLY) {
        if (log_min_level == LOG || log_min_level <= ERROR) return true;
    } else if (log_min_level == LOG) { /* elevel != LOG */
        if (elevel >= FATAL) return true;
    } /* Neither is LOG */ else if (elevel >= log_min_level) return true;
    return false;
}

static char *PQerrorMessageMy(const PGconn *conn) {
    char *err = PQerrorMessage(conn);
    int len;
    if (!err) return err;
    len = strlen(err);
    if (!len) return err;
    if (err[len - 1] == '\n') err[len - 1] = '\0';
    return err;
}

static char *PQresultErrorMessageMy(const PGresult *res) {
    char *err = PQresultErrorMessage(res);
    int len;
    if (!err) return err;
    len = strlen(err);
    if (!len) return err;
    if (err[len - 1] == '\n') err[len - 1] = '\0';
    return err;
}

static const char *work_status(Task *task) {
    switch (PQstatus(task->conn)) {
        case CONNECTION_AUTH_OK: return "CONNECTION_AUTH_OK";
        case CONNECTION_AWAITING_RESPONSE: return "CONNECTION_AWAITING_RESPONSE";
        case CONNECTION_BAD: return "CONNECTION_BAD";
        case CONNECTION_CHECK_STANDBY: return "CONNECTION_CHECK_STANDBY";
        case CONNECTION_CHECK_TARGET: return "CONNECTION_CHECK_TARGET";
        case CONNECTION_CHECK_WRITABLE: return "CONNECTION_CHECK_WRITABLE";
        case CONNECTION_CONSUME: return "CONNECTION_CONSUME";
        case CONNECTION_GSS_STARTUP: return "CONNECTION_GSS_STARTUP";
        case CONNECTION_MADE: return "CONNECTION_MADE";
        case CONNECTION_NEEDED: return "CONNECTION_NEEDED";
        case CONNECTION_OK: return "CONNECTION_OK";
        case CONNECTION_SETENV: return "CONNECTION_SETENV";
        case CONNECTION_SSL_STARTUP: return "CONNECTION_SSL_STARTUP";
        case CONNECTION_STARTED: return "CONNECTION_STARTED";
    }
    return "";
}

static void work_check(Work *work) {
    static SPI_plan *plan = NULL;
    static const char *command = SQL(
        WITH j AS (
            SELECT  COALESCE(COALESCE(j.user, data), current_setting('pg_task.default_user', false)) AS user,
                    COALESCE(COALESCE(data, j.user), current_setting('pg_task.default_data', false)) AS data,
                    schema,
                    COALESCE(j.table, current_setting('pg_task.default_table', false)) AS table,
                    COALESCE(reset, current_setting('pg_task.default_reset', false)::int4) AS reset,
                    COALESCE(timeout, current_setting('pg_task.default_timeout', false)::int4) AS timeout,
                    COALESCE(count, current_setting('pg_task.default_count', false)::int4) AS count,
                    EXTRACT(epoch FROM COALESCE(live, current_setting('pg_task.default_live', false)::interval))::int8 AS live
            FROM    json_populate_recordset(NULL::record, current_setting('pg_task.json', false)::json) AS j ("user" text, data text, schema text, "table" text, reset int4, timeout int4, count int4, live interval)
        ) SELECT    DISTINCT j.* FROM j
        inner JOIN  pg_user AS u ON usename = j.user
        inner JOIN  pg_database AS d ON datname = data AND NOT datistemplate AND datallowconn AND usesysid = datdba
        WHERE       j.user = current_user AND data = current_catalog AND schema IS NOT DISTINCT FROM current_setting('pg_task.schema', true) AND j.table = current_setting('pg_task.table', false) AND reset = current_setting('pg_task.reset', false)::int4 AND timeout = current_setting('pg_task.timeout', false)::int4
    );
    if (ShutdownRequestPending) return;
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_SELECT, true);
    if (!SPI_tuptable->numvals) ShutdownRequestPending = true;
    SPI_finish_my();
}

static void work_command(Task *task, PGresult *result) {
    if (task->skip) { task->skip--; return; }
    if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
    appendStringInfo(&task->output, "%s%s", task->output.len ? "\n" : "", PQcmdStatus(result));
}

static void work_edata(Task *task, const char *filename, int lineno, const char *funcname, const char *message) {
    ErrorData edata;
    MemSet(&edata, 0, sizeof(edata));
    edata.elevel = FATAL;
    edata.output_to_server = work_is_log_level_output(edata.elevel, log_min_messages);
    edata.filename = filename;
    edata.lineno = lineno;
    edata.funcname = funcname;
    edata.domain = TEXTDOMAIN ? TEXTDOMAIN : PG_TEXTDOMAIN("postgres");
    edata.context_domain = edata.domain;
    edata.sqlerrcode = ERRCODE_ADMIN_SHUTDOWN;
    edata.message = (char *)message;
    edata.message_id = edata.message;
    task_error(task, &edata);
    task_done(task);
}

static void work_event(Work *work, WaitEventSet *set) {
    dlist_mutable_iter iter;
    AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
    AddWaitEventToSet(set, WL_POSTMASTER_DEATH, PGINVALID_SOCKET, NULL, NULL);
    dlist_foreach_modify(iter, &work->head) {
        Task *task = dlist_container(Task, node, iter.cur);
        AddWaitEventToSet(set, task->event, PQsocket(task->conn), NULL, task);
    }
}

static void work_fail(Task *task, PGresult *result) {
    char *value = NULL;
    if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
    if (!task->error.data) initStringInfoMy(TopMemoryContext, &task->error);
    if ((value = PQresultErrorField(result, PG_DIAG_SEVERITY))) appendStringInfo(&task->error, "%sseverity%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED))) appendStringInfo(&task->error, "%sseverity_nonlocalized%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SQLSTATE))) appendStringInfo(&task->error, "%ssqlstate%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY))) appendStringInfo(&task->error, "%smessage_primary%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL))) appendStringInfo(&task->error, "%smessage_detail%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_HINT))) appendStringInfo(&task->error, "%smessage_hint%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_STATEMENT_POSITION))) appendStringInfo(&task->error, "%sstatement_position%s%c%s", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_INTERNAL_POSITION))) appendStringInfo(&task->error, "%sinternal_position%s%c%s", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_INTERNAL_QUERY))) appendStringInfo(&task->error, "%sinternal_query%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_CONTEXT))) appendStringInfo(&task->error, "%scontext%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SCHEMA_NAME))) appendStringInfo(&task->error, "%sschema_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_TABLE_NAME))) appendStringInfo(&task->error, "%stable_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_COLUMN_NAME))) appendStringInfo(&task->error, "%scolumn_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_DATATYPE_NAME))) appendStringInfo(&task->error, "%sdatatype_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_CONSTRAINT_NAME))) appendStringInfo(&task->error, "%sconstraint_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_FILE))) appendStringInfo(&task->error, "%ssource_file%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_LINE))) appendStringInfo(&task->error, "%ssource_line%s%c%s", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_FUNCTION))) appendStringInfo(&task->error, "%ssource_function%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if (value) appendStringInfo(&task->output, SQL(%sROLLBACK), task->output.len ? "\n" : "");
    task->skip++;
    task->fail = true;
}

static void work_free(Task *task) {
    if (task->group) pfree(task->group);
    if (task->null) pfree(task->null);
    if (task->remote) pfree(task->remote);
    if (task->input) pfree(task->input);
    if (task->output.data) pfree(task->output.data);
    if (task->error.data) pfree(task->error.data);
    pfree(task);
}

static void work_pids(Work *work) {
    dlist_mutable_iter iter;
    int nelems = 0;
    StringInfoData buf;
    dlist_foreach_modify(iter, &work->head) nelems++;
    if (work->pids) pfree(work->pids);
    work->pids = NULL;
    if (!nelems) return;
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfoString(&buf, "{");
    nelems = 0;
    dlist_foreach_modify(iter, &work->head) {
        Task *task = dlist_container(Task, node, iter.cur);
        if (!task->pid) continue;
        if (nelems) appendStringInfoString(&buf, ",");
        appendStringInfo(&buf, "%i", task->pid);
        nelems++;
    }
    appendStringInfoString(&buf, "}");
    work->pids = buf.data;
    D1("pids = %s", work->pids);
}

static void work_finish(Task *task) {
    Work *work = task->work;
    dlist_delete(&task->node);
    PQfinish(task->conn);
    work_pids(work);
    work_free(task);
}

static void work_error(Task *task, const char *msg, const char *err, bool finish) {
    if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
    if (!task->error.data) initStringInfoMy(TopMemoryContext, &task->error);
    appendStringInfo(&task->error, "%s%s", task->error.len ? "\n" : "", msg);
    if (err && strlen(err)) appendStringInfo(&task->error, " and %s", err);
    W("%li: %s", task->id, task->error.data);
    appendStringInfo(&task->output, SQL(%sROLLBACK), task->output.len ? "\n" : "");
    task->fail = true;
    task->skip++;
    task_done(task);
    finish ? work_finish(task) : work_free(task);
}

static int work_nevents(Work *work) {
    dlist_mutable_iter iter;
    int nevents = 0;
    dlist_foreach_modify(iter, &work->head) {
        Task *task = dlist_container(Task, node, iter.cur);
        if (PQstatus(task->conn) == CONNECTION_BAD) { work_error(task, "PQstatus == CONNECTION_BAD", PQerrorMessageMy(task->conn), true); continue; }
        if (PQsocket(task->conn) == PGINVALID_SOCKET) { work_error(task, "PQsocket == PGINVALID_SOCKET", PQerrorMessageMy(task->conn), true); continue; }
        nevents++;
    }
    return nevents;
}

static void work_fini(Work *work) {
    dlist_mutable_iter iter;
    StringInfoData buf;
    D1("user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i, count = %i, live = %li", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, work->conf.reset, work->conf.timeout, work->conf.count, work->conf.live);
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, "terminating background worker \"%s\" due to administrator command", MyBgworkerEntry->bgw_type);
    dlist_foreach_modify(iter, &work->head) {
        Task *task = dlist_container(Task, node, iter.cur);
        if (!PQrequestCancel(task->conn)) work_error(task, buf.data, PQerrorMessageMy(task->conn), true); else {
            work_edata(task, __FILE__, __LINE__, __func__, buf.data);
            work_finish(task);
        }
    }
    pfree(buf.data);
    if (ShutdownRequestPending) return;
    conf_work(&work->conf, work->data, work->user);
}

static void work_index(Work *work, const char *index) {
    StringInfoData buf, name, idx;
    List *names;
    RelationData *relation;
    const RangeVar *rangevar;
    const char *name_quote;
    const char *index_quote = quote_identifier(index);
    const char *schema_quote = work->conf.schema ? quote_identifier(work->conf.schema) : NULL;
    D1("user = %s, data = %s, schema = %s, table = %s, index = %s, schema_table = %s", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, index, work->schema_table);
    initStringInfoMy(TopMemoryContext, &name);
    appendStringInfo(&name, "%s_%s_idx", work->conf.table, index);
    name_quote = quote_identifier(name.data);
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, SQL(CREATE INDEX %s ON %s USING btree (%s)), name_quote, work->schema_table, index_quote);
    initStringInfoMy(TopMemoryContext, &idx);
    if (work->conf.schema) appendStringInfo(&idx, "%s.", schema_quote);
    appendStringInfoString(&idx, name_quote);
    names = stringToQualifiedNameList(idx.data);
    rangevar = makeRangeVarFromNameList(names);
    SPI_connect_my(buf.data);
    if (!OidIsValid(RangeVarGetRelid(rangevar, NoLock, true))) {
        SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    } else if ((relation = relation_openrv_extended(rangevar, AccessShareLock, true))) {
        if (relation->rd_index && relation->rd_index->indrelid != work->table) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
        relation_close(relation, AccessShareLock);
    }
    SPI_commit_my();
    SPI_finish_my();
    pfree((void *)rangevar);
    list_free_deep(names);
    pfree(buf.data);
    pfree(name.data);
    pfree(idx.data);
    if (work->conf.schema && schema_quote && work->conf.schema != schema_quote) pfree((void *)schema_quote);
    if (name_quote != name.data) pfree((void *)name_quote);
    if (index_quote != index) pfree((void *)index_quote);
}

static void work_reload(Work *work) {
    ConfigReloadPending = false;
    ProcessConfigFile(PGC_SIGHUP);
    work_check(work);
}

static void work_latch(Work *work) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
    if (ConfigReloadPending) work_reload(work);
}

static bool work_busy(Task *task, int event) {
    if (PQisBusy(task->conn)) { W("%li: PQisBusy", task->id); task->event = event; return false; }
    return true;
}

static bool work_consume(Task *task) {
    if (!PQconsumeInput(task->conn)) { work_error(task, "!PQconsumeInput", PQerrorMessageMy(task->conn), true); return false; }
    return true;
}

static bool work_flush(Task *task) {
    switch (PQflush(task->conn)) {
        case 0: break;
        case 1: D1("%li: PQflush == 1", task->id); task->event = WL_SOCKET_MASK; return false;
        case -1: work_error(task, "PQflush == -1", PQerrorMessageMy(task->conn), true); return false;
    }
    return true;
}

static bool work_consume_flush_busy(Task *task) {
    if (!work_consume(task)) return false;
    if (!work_flush(task)) return false;
    if (!work_busy(task, WL_SOCKET_READABLE)) return false;
    return true;
}

static void work_readable(Task *task) {
    if (PQstatus(task->conn) == CONNECTION_OK) if (!work_consume_flush_busy(task)) return;
    task->socket(task);
}

static void work_done(Task *task) {
    if (PQstatus(task->conn) == CONNECTION_OK && PQtransactionStatus(task->conn) != PQTRANS_IDLE) {
        task->socket = work_done;
        if (!work_busy(task, WL_SOCKET_WRITEABLE)) return;
        if (!PQsendQuery(task->conn, SQL(COMMIT))) { work_error(task, "!PQsendQuery", PQerrorMessageMy(task->conn), false); return; }
        if (!work_flush(task)) return;
        task->event = WL_SOCKET_READABLE;
        return;
    }
    if (task_done(task)) { work_finish(task); return; }
    D1("id = %li, repeat = %s, delete = %s, live = %s", task->id, task->repeat ? "true" : "false", task->delete ? "true" : "false", task->live ? "true" : "false");
    if (task->repeat) task_repeat(task);
    if (task->delete && !task->output.data) task_delete(task);
    if (task->output.data) pfree(task->output.data);
    task->output.data = NULL;
    if (task->error.data) pfree(task->error.data);
    task->error.data = NULL;
    if (ShutdownRequestPending) task->live = false;
    (PQstatus(task->conn) != CONNECTION_OK || !task->live || task_live(task)) ? work_finish(task) : work_query(task);
}

static void work_schema(Work *work) {
    StringInfoData buf;
    List *names;
    const char *schema_quote = quote_identifier(work->conf.schema);
    D1("user = %s, data = %s, schema = %s, table = %s", work->user, work->data, work->conf.schema, work->conf.table);
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, SQL(CREATE SCHEMA %s), schema_quote);
    names = stringToQualifiedNameList(schema_quote);
    SPI_connect_my(buf.data);
    if (!OidIsValid(get_namespace_oid(strVal(linitial(names)), true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    work->schema = get_namespace_oid(strVal(linitial(names)), false);
    SPI_commit_my();
    SPI_finish_my();
    list_free_deep(names);
    if (schema_quote != work->conf.schema) pfree((void *)schema_quote);
    pfree(buf.data);
    set_config_option("pg_task.schema", work->conf.schema, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
}

static void work_success(Task *task, PGresult *result) {
    if (task->length == 1 && !PQntuples(result)) return;
    if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
    if (task->header && (task->length > 1 || PQnfields(result) > 1)) {
        if (task->output.len) appendStringInfoString(&task->output, "\n");
        for (int col = 0; col < PQnfields(result); col++) {
            const char *value = PQfname(result, col);
            if (col > 0) appendStringInfoChar(&task->output, task->delimiter);
            if (task->quote) appendStringInfoChar(&task->output, task->quote);
            if (task->escape) init_escape(&task->output, value, strlen(value), task->escape);
            else appendStringInfoString(&task->output, value);
            if (task->append && !strstr(value, "::")) {
                Oid oid = PQftype(result, col);
                const char *type = PQftypeMy(oid);
                if (task->escape) init_escape(&task->output, "::", sizeof("::") - 1, task->escape);
                else appendStringInfoString(&task->output, "::");
                if (type) {
                    if (task->escape) init_escape(&task->output, type, strlen(type), task->escape);
                    else appendStringInfoString(&task->output, type);
                } else appendStringInfo(&task->output, "%i", oid);
            }
            if (task->quote) appendStringInfoChar(&task->output, task->quote);
        }
    }
    for (int row = 0; row < PQntuples(result); row++) {
        if (task->output.len) appendStringInfoString(&task->output, "\n");
        for (int col = 0; col < PQnfields(result); col++) {
            const char *value = PQgetvalue(result, row, col);
            int len = PQgetlength(result, row, col);
            if (col > 0) appendStringInfoChar(&task->output, task->delimiter);
            if (PQgetisnull(result, row, col)) appendStringInfoString(&task->output, task->null); else {
                if (!init_oid_is_string(PQftype(result, col)) && task->string) {
                    if (len) appendStringInfoString(&task->output, value);
                } else {
                    if (task->quote) appendStringInfoChar(&task->output, task->quote);
                    if (len) {
                        if (task->escape) init_escape(&task->output, value, len, task->escape);
                        else appendStringInfoString(&task->output, value);
                    }
                    if (task->quote) appendStringInfoChar(&task->output, task->quote);
                }
            }
        }
    }
}

static void work_result(Task *task) {
    for (PGresult *result; PQstatus(task->conn) == CONNECTION_OK && (result = PQgetResult(task->conn)); ) {
        switch (PQresultStatus(result)) {
            case PGRES_COMMAND_OK: work_command(task, result); break;
            case PGRES_FATAL_ERROR: W("%li: PQresultStatus == PGRES_FATAL_ERROR and %s", task->id, PQresultErrorMessageMy(result)); work_fail(task, result); break;
            case PGRES_TUPLES_OK: work_success(task, result); break;
            default: D1("%li: %s", task->id, PQresStatus(PQresultStatus(result))); break;
        }
        PQclear(result);
        if (!work_consume_flush_busy(task)) return;
    }
    work_done(task);
}

static bool work_input(Task *task) {
    StringInfoData buf;
    List *list;
    if (ShutdownRequestPending) return true;
    if (task_work(task)) return true;
    D1("id = %li, timeout = %i, input = %s, count = %i", task->id, task->timeout, task->input, task->count);
    PG_TRY();
        list = pg_parse_query(task->input);
        task->length = list_length(list);
        list_free_deep(list);
    PG_CATCH();
        FlushErrorState();
    PG_END_TRY();
    initStringInfoMy(TopMemoryContext, &buf);
    task->skip = 0;
    appendStringInfo(&buf, SQL(SET "pg_task.id" = %li;), task->id);
    task->skip++;
    if (task->timeout) {
        appendStringInfo(&buf, SQL(SET "statement_timeout" = %i;), task->timeout);
        task->skip++;
    }
    if (task->append) {
        appendStringInfoString(&buf, SQL(SET "config.append_type_to_column_name" = true;));
        task->skip++;
    }
    appendStringInfoString(&buf, task->input);
    pfree(task->input);
    task->input = buf.data;
    return false;
}

static void work_query(Task *task) {
    if (ShutdownRequestPending) return;
    task->socket = work_query;
    if (!work_busy(task, WL_SOCKET_WRITEABLE)) return;
    if (work_input(task)) { work_finish(task); return; }
    if (!PQsendQuery(task->conn, task->input)) { work_error(task, "!PQsendQuery", PQerrorMessageMy(task->conn), false); return; }
    task->socket = work_result;
    if (!work_flush(task)) return;
    task->event = WL_SOCKET_READABLE;
}

static void work_connect(Task *task) {
    bool connected = false;
    switch (PQstatus(task->conn)) {
        case CONNECTION_BAD: D1("%li: PQstatus == CONNECTION_BAD", task->id); work_error(task, "PQstatus == CONNECTION_BAD", PQerrorMessageMy(task->conn), true); return;
        case CONNECTION_OK: D1("%li: PQstatus == CONNECTION_OK", task->id); connected = true; break;
        default: break;
    }
    if (!connected) switch (PQconnectPoll(task->conn)) {
        case PGRES_POLLING_ACTIVE: D1("%li: PQconnectPoll == PGRES_POLLING_ACTIVE and %s", task->id, work_status(task)); break;
        case PGRES_POLLING_FAILED: D1("%li: PQconnectPoll == PGRES_POLLING_FAILED and %s", task->id, work_status(task)); work_error(task, "PQconnectPoll == PGRES_POLLING_FAILED", PQerrorMessageMy(task->conn), true); return;
        case PGRES_POLLING_OK: D1("%li: PQconnectPoll == PGRES_POLLING_OK and %s", task->id, work_status(task)); connected = true; break;
        case PGRES_POLLING_READING: D1("%li: PQconnectPoll == PGRES_POLLING_READING and %s", task->id, work_status(task)); task->event = WL_SOCKET_READABLE; break;
        case PGRES_POLLING_WRITING: D1("%li: PQconnectPoll == PGRES_POLLING_WRITING and %s", task->id, work_status(task)); task->event = WL_SOCKET_WRITEABLE; break;
    }
    if (connected) {
        Work *work = task->work;
        if(!(task->pid = PQbackendPID(task->conn))) { work_error(task, "!PQbackendPID", PQerrorMessageMy(task->conn), true); return; }
        work_pids(work);
        work_query(task);
    }
}

static void work_remote(Task *task) {
    bool password = false;
    char *err;
    char *options = NULL;
    const char **keywords;
    const char **values;
    int arg = 3;
    PQconninfoOption *opts = PQconninfoParse(task->remote, &err);
    StringInfoData buf, buf2;
    Work *work = task->work;
    D1("id = %li, group = %s, remote = %s, max = %i, oid = %i", task->id, task->group, task->remote ? task->remote : default_null, task->max, work->table);
    if (!opts) { work_error(task, "!PQconninfoParse", err, false); if (err) PQfreemem(err); return; }
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        D1("%s = %s", opt->keyword, opt->val);
        if (!strcmp(opt->keyword, "password")) password = true;
        if (!strcmp(opt->keyword, "fallback_application_name")) continue;
        if (!strcmp(opt->keyword, "application_name")) continue;
        if (!strcmp(opt->keyword, "options")) { options = opt->val; continue; }
        arg++;
    }
    if (!superuser() && !password) { work_error(task, "!superuser && !password", NULL, false); PQconninfoFree(opts); return; }
    keywords = MemoryContextAlloc(TopMemoryContext, arg * sizeof(*keywords));
    values = MemoryContextAlloc(TopMemoryContext, arg * sizeof(*values));
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, "pg_task %s%s%s %s", work->conf.schema ? work->conf.schema : "", work->conf.schema ? " " : "", work->conf.table, task->group);
    arg = 0;
    keywords[arg] = "application_name";
    values[arg] = buf.data;
    initStringInfoMy(TopMemoryContext, &buf2);
    if (options) appendStringInfoString(&buf2, options);
    appendStringInfo(&buf2, "%s-c pg_task.data=%s", buf2.len ? " " : "", work->data);
    appendStringInfo(&buf2, " -c pg_task.user=%s", work->user);
    if (work->conf.schema) appendStringInfo(&buf2, " -c pg_task.schema=%s", work->conf.schema);
    appendStringInfo(&buf2, " -c pg_task.table=%s", work->conf.table);
    appendStringInfo(&buf2, " -c pg_task.oid=%i", work->table);
    appendStringInfo(&buf2, " -c pg_task.group=%s", task->group);
    arg++;
    keywords[arg] = "options";
    values[arg] = buf2.data;
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        if (!strcmp(opt->keyword, "fallback_application_name")) continue;
        if (!strcmp(opt->keyword, "application_name")) continue;
        if (!strcmp(opt->keyword, "options")) continue;
        arg++;
        keywords[arg] = opt->keyword;
        values[arg] = opt->val;
    }
    arg++;
    keywords[arg] = NULL;
    values[arg] = NULL;
    task->event = WL_SOCKET_MASK;
    task->socket = work_connect;
    task->start = GetCurrentTimestamp();
    dlist_push_head(&work->head, &task->node);
    if (!(task->conn = PQconnectStartParams(keywords, values, false))) work_error(task, "!PQconnectStartParams", PQerrorMessageMy(task->conn), true);
    else if (PQstatus(task->conn) == CONNECTION_BAD) work_error(task, "PQstatus == CONNECTION_BAD", PQerrorMessageMy(task->conn), true);
    else if (!PQisnonblocking(task->conn) && PQsetnonblocking(task->conn, true) == -1) work_error(task, "PQsetnonblocking == -1", PQerrorMessageMy(task->conn), true);
    else if (!superuser() && !PQconnectionUsedPassword(task->conn)) work_error(task, "!superuser && !PQconnectionUsedPassword", PQerrorMessageMy(task->conn), true);
    else if (PQclientEncoding(task->conn) != GetDatabaseEncoding()) PQsetClientEncoding(task->conn, GetDatabaseEncodingName());
    pfree(buf.data);
    pfree(buf2.data);
    pfree(keywords);
    pfree(values);
    PQconninfoFree(opts);
}

static void work_table(Work *work) {
    StringInfoData buf;
    List *names;
    const RangeVar *rangevar;
    D1("user = %s, data = %s, schema = %s, table = %s, schema_table = %s, schema_type = %s", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, work->schema_table, work->schema_type);
    set_config_option("pg_task.table", work->conf.table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, SQL(
        CREATE TABLE %1$s (
            id bigserial NOT NULL PRIMARY KEY,
            parent int8 DEFAULT current_setting('pg_task.id', true)::int8 REFERENCES %1$s (id) MATCH SIMPLE ON UPDATE CASCADE ON DELETE SET NULL,
            dt timestamptz NOT NULL DEFAULT current_timestamp,
            start timestamptz,
            stop timestamptz,
            live interval,
            timeout interval,
            repeat interval,
            count int4,
            max int4,
            pid int4,
            state %2$s NOT NULL DEFAULT 'PLAN'::%2$s,
            delete boolean NOT NULL DEFAULT false,
            drift boolean NOT NULL DEFAULT true,
            append boolean NOT NULL DEFAULT false,
            header boolean NOT NULL DEFAULT true,
            string boolean NOT NULL DEFAULT true,
            delimiter "char" NOT NULL DEFAULT '\t',
            escape "char",
            quote "char",
            error text,
            "group" text NOT NULL DEFAULT 'group',
            input text NOT NULL,
            "null" text NOT NULL DEFAULT '\\N',
            output text,
            remote text
        )
    ), work->schema_table, work->schema_type);
    names = stringToQualifiedNameList(work->schema_table);
    rangevar = makeRangeVarFromNameList(names);
    SPI_connect_my(buf.data);
    if (!OidIsValid(RangeVarGetRelid(rangevar, NoLock, true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    work->table = RangeVarGetRelid(rangevar, NoLock, false);
    SPI_commit_my();
    SPI_finish_my();
    pfree((void *)rangevar);
    list_free_deep(names);
    set_config_option("pg_task.table", work->conf.table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->table);
    set_config_option("pg_task.oid", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pfree(buf.data);
}

static void work_task(Task *task) {
    BackgroundWorkerHandle *handle = NULL;
    BackgroundWorker worker;
    pid_t pid;
    size_t len = 0;
    Work *work = task->work;
    D1("user = %s, data = %s, schema = %s, table = %s, id = %li, group = %s, max = %i, oid = %i", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, task->id, task->group, task->max, work->table);
    MemSet(&worker, 0, sizeof(worker));
    if (strlcpy(worker.bgw_function_name, "task", sizeof(worker.bgw_function_name)) >= sizeof(worker.bgw_function_name)) { work_error(task, "strlcpy", NULL, false); return; }
    if (strlcpy(worker.bgw_library_name, "pg_task", sizeof(worker.bgw_library_name)) >= sizeof(worker.bgw_library_name)) { work_error(task, "strlcpy", NULL, false); return; }
    if (snprintf(worker.bgw_type, sizeof(worker.bgw_type) - 1, "pg_task %s%s%s %s", work->conf.schema ? work->conf.schema : "", work->conf.schema ? " " : "", work->conf.table, task->group) >= sizeof(worker.bgw_type) - 1) { work_error(task, "snprintf", NULL, false); return; }
    if (snprintf(worker.bgw_name, sizeof(worker.bgw_name) - 1, "%s %s %s", work->user, work->data, worker.bgw_type) >= sizeof(worker.bgw_name) - 1) { work_error(task, "snprintf", NULL, false); return; }
#define X(src, serialize, deserialize) serialize(src);
    WORK
#undef X
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_main_arg = Int64GetDatum(task->id);
    worker.bgw_notify_pid = MyProcPid;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    if (init_check_ascii_all(&worker)) { work_error(task, "init_check_ascii_all", NULL, false); return; }
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) { work_error(task, "RegisterDynamicBackgroundWorker", NULL, false); if (handle) pfree(handle); return; }
    switch (WaitForBackgroundWorkerStartup(handle, &pid)) {
        case BGWH_NOT_YET_STARTED: work_error(task, "WaitForBackgroundWorkerStartup == BGWH_NOT_YET_STARTED", NULL, false); pfree(handle); return;
        case BGWH_POSTMASTER_DIED: work_error(task, "WaitForBackgroundWorkerStartup == BGWH_POSTMASTER_DIED", NULL, false); pfree(handle); return;
        case BGWH_STARTED: break;
        case BGWH_STOPPED: work_error(task, "WaitForBackgroundWorkerStartup == BGWH_STOPPED", NULL, false); pfree(handle); return;
    }
    pfree(handle);
    work_free(task);
}

static void work_type(Work *work) {
    StringInfoData buf;
    Oid type = InvalidOid;
    int32 typmod;
    const char *schema_quote = work->conf.schema ? quote_identifier(work->conf.schema) : NULL;
    D1("user = %s, data = %s, schema = %s, table = %s", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table);
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, SQL(CREATE TYPE %s AS ENUM ('PLAN', 'TAKE', 'WORK', 'DONE', 'FAIL', 'STOP')), work->schema_type);
    SPI_connect_my(buf.data);
    parseTypeString(work->schema_type, &type, &typmod, true);
    if (!OidIsValid(type)) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    SPI_commit_my();
    SPI_finish_my();
    if (work->conf.schema && schema_quote && work->conf.schema != schema_quote) pfree((void *)schema_quote);
    pfree(buf.data);
}

static void work_conf(Work *work) {
    const char *schema_quote = work->conf.schema ? quote_identifier(work->conf.schema) : NULL;
    const char *table_quote = quote_identifier(work->conf.table);
    StringInfoData buf;
    initStringInfoMy(TopMemoryContext, &buf);
    if (work->conf.schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, table_quote);
    if (work->schema_table) pfree(work->schema_table);
    work->schema_table = buf.data;
    initStringInfoMy(TopMemoryContext, &buf);
    if (work->conf.schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, "state");
    if (work->schema_type) pfree(work->schema_type);
    work->schema_type = buf.data;
    if (work->conf.schema && schema_quote && work->conf.schema != schema_quote) pfree((void *)schema_quote);
    if (work->conf.table != table_quote) pfree((void *)table_quote);
    D1("user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i, count = %i, live = %li, schema_table = %s, schema_table = %s", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, work->conf.reset, work->conf.timeout, work->conf.count, work->conf.live, work->schema_table, work->schema_type);
    if (work->conf.schema) work_schema(work);
    work_type(work);
    work_table(work);
    work_index(work, "dt");
    work_index(work, "parent");
    work_index(work, "state");
    set_config_option("pg_task.data", work->data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.user", work->user, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, "%i", work->conf.reset);
    set_config_option("pg_task.reset", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->conf.timeout);
    set_config_option("pg_task.timeout", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pfree(buf.data);
    dlist_init(&work->head);
}

static void work_init(Work *work) {
    char *p = MyBgworkerEntry->bgw_extra;
    MemoryContextData *oldcontext = CurrentMemoryContext;
#define X(type, name, get, serialize, deserialize) deserialize(work->conf.name);
    CONF
#undef X
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(work->conf.data, work->conf.user, 0);
    pgstat_report_appname(MyBgworkerEntry->bgw_type);
    process_session_preload_libraries();
    StartTransactionCommand();
    MemoryContextSwitchTo(oldcontext);
    work->data = get_database_name(work->conf.data);
    work->user = GetUserNameFromId(work->conf.user, false);
    CommitTransactionCommand();
    MemoryContextSwitchTo(oldcontext);
    if (!MyProcPort && !(MyProcPort = (Port *) calloc(1, sizeof(Port)))) E("!calloc");
    if (!MyProcPort->remote_host) MyProcPort->remote_host = "[local]";
    if (!MyProcPort->user_name) MyProcPort->user_name = work->user;
    if (!MyProcPort->database_name) MyProcPort->database_name = work->data;
    set_config_option("application_name", MyBgworkerEntry->bgw_type, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    D1("user_oid = %i, data_oid = %i, user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i, count = %i, live = %li", work->conf.user, work->conf.data, work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, work->conf.reset, work->conf.timeout, work->conf.count, work->conf.live);
    work_conf(work);
}

static void work_update(Work *work) {
    static Oid argtypes[] = {TEXTOID};
    Datum values[] = {work->pids ? CStringGetTextDatum(work->pids) : (Datum)NULL};
    char nulls[] = {work->pids ? ' ' : 'n'};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            WITH s AS (
                SELECT id FROM %1$s AS t WHERE dt < current_timestamp - concat_ws(' ', (current_setting('pg_task.reset', false)::int4 * current_setting('pg_task.timeout', false)::int4)::text, 'msec')::interval AND state IN ('TAKE'::%2$s, 'WORK'::%2$s) AND pid NOT IN (
                    SELECT      pid FROM pg_stat_activity
                    WHERE       datname = current_catalog AND usename = current_user AND application_name = concat_ws(' ', 'pg_task', current_setting('pg_task.schema', true), current_setting('pg_task.table', false), t.group)
                    UNION       SELECT UNNEST($1::int4[])
                ) FOR UPDATE SKIP LOCKED
            ) UPDATE %1$s AS u SET state = 'PLAN'::%2$s FROM s WHERE u.id = s.id RETURNING u.id
        ), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, nulls, SPI_OK_UPDATE_RETURNING, true);
    for (uint64 row = 0; row < SPI_tuptable->numvals; row++) {
        int64 id = DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "id", false));
        W("row = %lu, id = %li", row, id);
    }
    SPI_finish_my();
    if (work->pids) pfree((void *)values[0]);
}

static void work_timeout(Work *work) {
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    work_update(work);
    if (!command) {
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            WITH s AS (WITH s AS (WITH s AS (WITH s AS (WITH s AS (
                SELECT      t.id, t.group, COALESCE(t.max, ~(1<<31)) AS max, a.pid FROM %1$s AS t
                LEFT JOIN   %1$s AS a ON a.state IN ('TAKE'::%2$s, 'WORK'::%2$s) AND t.group = a.group
                WHERE       t.state = 'PLAN'::%2$s AND t.dt + concat_ws(' ', (CASE WHEN t.max < 0 THEN -t.max ELSE 0 END)::text, 'msec')::interval <= current_timestamp
            ) SELECT id, s.group, CASE WHEN max > 0 THEN max ELSE 1 END - count(pid) AS count FROM s GROUP BY id, s.group, max
            ) SELECT array_agg(id ORDER BY id) AS id, s.group, count FROM s WHERE count > 0 GROUP BY s.group, count
            ) SELECT unnest(id[:count]) AS id, s.group, count FROM s ORDER BY count DESC
            ) SELECT id FROM s INNER JOIN %1$s USING (id) FOR UPDATE SKIP LOCKED
            ) UPDATE %1$s AS u SET state = 'TAKE'::%2$s FROM s WHERE u.id = s.id RETURNING u.id, u.group, u.remote, COALESCE(u.max, ~(1<<31)) AS max, start IS NULL AS start_isnull, stop IS NULL AS stop_isnull, pid IS NULL AS pid_isnull
        ), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_UPDATE_RETURNING, true);
    for (uint64 row = 0; row < SPI_tuptable->numvals; row++) {
        Task *task = MemoryContextAllocZero(TopMemoryContext, sizeof(*task));
        bool pid_isnull = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "pid_isnull", false));
        bool start_isnull = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "start_isnull", false));
        bool stop_isnull = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "stop_isnull", false));
        task->group = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "group", false));
        task->id = DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "id", false));
        task->max = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "max", false));
        task->remote = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "remote", true));
        task->work = work;
        D1("row = %lu, id = %li, group = %s, remote = %s, max = %i, start_isnull = %s, stop_isnull = %s, pid_isnull = %s", row, task->id, task->group, task->remote ? task->remote : default_null, task->max, start_isnull ? "true" : "false", stop_isnull ? "true" : "false", pid_isnull ? "true" : "false");
        if (!start_isnull) { work_error(task, "!start_isnull", NULL, false); continue; }
        if (!stop_isnull) { work_error(task, "!stop_isnull", NULL, false); continue; }
        if (!pid_isnull) { work_error(task, "!pid_isnull", NULL, false); continue; }
        task->remote ? work_remote(task) : work_task(task);
    }
    if (work->conf.count) work->count += SPI_tuptable->numvals;
    SPI_finish_my();
}

static void work_writeable(Task *task) {
    if (PQstatus(task->conn) == CONNECTION_OK) if (!work_flush(task)) return;
    task->socket(task);
}

void work(Datum main_arg) {
    instr_time cur_time;
    instr_time start_time;
    long cur_timeout = -1;
    Work *work = MemoryContextAllocZero(TopMemoryContext, sizeof(*work));
    work_init(work);
    if (!init_data_user_table_lock(MyDatabaseId, GetUserId(), work->table)) W("!init_data_user_table_lock(%i, %i, %i)", MyDatabaseId, GetUserId(), work->table); else while (!ShutdownRequestPending) {
        int nevents = 2 + work_nevents(work);
        WaitEvent *events = MemoryContextAllocZero(TopMemoryContext, nevents * sizeof(*events));
        WaitEventSet *set = CreateWaitEventSet(TopMemoryContext, nevents);
        work_event(work, set);
        if (cur_timeout <= 0) {
            INSTR_TIME_SET_CURRENT(start_time);
            cur_timeout = work->conf.timeout;
        }
        nevents = WaitEventSetWait(set, cur_timeout, events, nevents, PG_WAIT_EXTENSION);
        for (int i = 0; i < nevents; i++) {
            WaitEvent *event = &events[i];
            if (event->events & WL_LATCH_SET) work_latch(work);
            if (event->events & WL_POSTMASTER_DEATH) ShutdownRequestPending = true;
            if (event->events & WL_SOCKET_READABLE) work_readable(event->user_data);
            if (event->events & WL_SOCKET_WRITEABLE) work_writeable(event->user_data);
        }
        if (work->conf.timeout >= 0) {
            INSTR_TIME_SET_CURRENT(cur_time);
            INSTR_TIME_SUBTRACT(cur_time, start_time);
            cur_timeout = work->conf.timeout - (long)INSTR_TIME_GET_MILLISEC(cur_time);
            if (cur_timeout <= 0) work_timeout(work);
        }
        FreeWaitEventSet(set);
        pfree(events);
        if (work->conf.count && work->count >= work->conf.count) break;
        if (work->conf.live && TimestampDifferenceExceeds(MyStartTimestamp, GetCurrentTimestamp(), work->conf.live * 1000)) break;
    }
    if (!init_data_user_table_unlock(MyDatabaseId, GetUserId(), work->table)) W("!init_data_user_table_unlock(%i, %i, %i)", MyDatabaseId, GetUserId(), work->table);
    work_fini(work);
    pfree(work);
}
