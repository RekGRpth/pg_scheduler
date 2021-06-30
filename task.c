#include "include.h"

extern bool xact_started;
extern char *default_null;

static void task_update(Task *task) {
    Work *work = task->work;
    static Oid argtypes[] = {TEXTOID};
    Datum values[] = {CStringGetTextDatum(task->group)};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            WITH s AS (
                SELECT id FROM %1$s AS t WHERE max < 0 AND dt < current_timestamp AND t.group = $1 AND state = 'PLAN'::%2$s FOR UPDATE SKIP LOCKED
            ) UPDATE %1$s AS u SET dt = current_timestamp FROM s WHERE u.id = s.id
        ), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_UPDATE, true);
    SPI_finish_my();
    pfree((void *)values[0]);
}

bool task_done(Task *task) {
    bool exit = false;
    Work *work = task->work;
    static Oid argtypes[] = {INT8OID, BOOLOID, TEXTOID, TEXTOID};
    Datum values[] = {Int64GetDatum(task->id), BoolGetDatum(task->fail = task->output.data ? task->fail : false), task->output.data ? CStringGetTextDatum(task->output.data) : (Datum)NULL, task->error.data ? CStringGetTextDatum(task->error.data) : (Datum)NULL};
    char nulls[] = {' ', ' ', task->output.data ? ' ' : 'n', task->error.data ? ' ' : 'n'};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    D1("id = %li, output = %s, error = %s, fail = %s", task->id, task->output.data ? task->output.data : default_null, task->error.data ? task->error.data : default_null, task->fail ? "true" : "false");
    task_update(task);
    if (!command) {
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            WITH s AS (
                SELECT id FROM %1$s WHERE id = $1 AND state IN ('TAKE'::%2$s, 'WORK'::%2$s) FOR UPDATE
            ) UPDATE %1$s AS u SET state = CASE WHEN $2 THEN 'FAIL'::%2$s ELSE 'DONE'::%2$s END, stop = current_timestamp, output = concat_ws('%3$s', NULLIF(output, ''), $3), error = concat_ws('%3$s', NULLIF(error, ''), $4) FROM s WHERE u.id = s.id
            RETURNING delete, repeat IS NOT NULL AND state IN ('DONE'::%2$s, 'FAIL'::%2$s) AS repeat, count IS NOT NULL OR live IS NOT NULL AS live
        ), work->schema_table, work->schema_type, "\n");
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, nulls, SPI_OK_UPDATE_RETURNING, true);
    if (SPI_tuptable->numvals != 1) {
        W("%li: SPI_tuptable->numvals != 1", task->id);
        exit = true;
    } else {
        task->delete = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "delete", false));
        task->repeat = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "repeat", false));
        task->live = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "live", false));
    }
    SPI_finish_my();
    if (task->output.data) pfree((void *)values[2]);
    if (task->error.data) pfree((void *)values[3]);
    if (task->null) pfree(task->null);
    task->null = NULL;
    if (task->lock && !init_table_id_unlock(work->table, task->id)) { W("!init_table_id_unlock(%i, %li)", work->table, task->id); exit = true; }
    task->lock = false;
    if (ShutdownRequestPending) exit = true;
    return exit;
}

bool task_live(Task *task) {
    bool exit = false;
    static Oid argtypes[] = {TEXTOID, TEXTOID, INT4OID, INT4OID, TIMESTAMPTZOID};
    Datum values[] = {CStringGetTextDatum(task->group), task->remote ? CStringGetTextDatum(task->remote) : (Datum)NULL, Int32GetDatum(task->max), Int32GetDatum(task->count), TimestampTzGetDatum(task->start)};
    char nulls[] = {' ', task->remote ? ' ' : 'n', ' ', ' ', ' '};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        Work *work = task->work;
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            WITH s AS (
                SELECT id FROM %1$s AS t WHERE state = 'PLAN'::%2$s AND dt <= current_timestamp AND t.group = $1 AND remote IS NOT DISTINCT FROM $2 AND COALESCE(max, ~(1<<31)) >= $3 AND CASE WHEN count IS NOT NULL AND live IS NOT NULL THEN count > $4 AND $5 + live > current_timestamp ELSE COALESCE(count, 0) > $4 OR $5 + COALESCE(live, '0 sec'::interval) > current_timestamp END
                ORDER BY COALESCE(max, ~(1<<31)) DESC LIMIT 1 FOR UPDATE SKIP LOCKED
            ) UPDATE %1$s AS u SET state = 'TAKE'::%2$s FROM s WHERE u.id = s.id RETURNING u.id
        ), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, nulls, SPI_OK_UPDATE_RETURNING, true);
    if (!SPI_tuptable->numvals) exit = true; else task->id = DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "id", false));
    SPI_finish_my();
    pfree((void *)values[0]);
    if (task->remote) pfree((void *)values[1]);
    return exit;
}

bool task_work(Task *task) {
    bool exit = false;
    Work *work = task->work;
    static Oid argtypes[] = {INT8OID, INT4OID};
    Datum values[] = {Int64GetDatum(task->id), Int32GetDatum(task->pid)};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (ShutdownRequestPending) return true;
    if (!init_table_id_lock(work->table, task->id)) { W("!init_table_id_lock(%i, %li)", work->table, task->id); return true; }
    task->lock = true;
    task->count++;
    D1("id = %li, group = %s, max = %i, oid = %i, count = %i, pid = %i", task->id, task->group, task->max, work->table, task->count, task->pid);
    if (!task->conn) {
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, "%li", task->id);
        set_config_option("pg_task.id", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
        pfree(buf.data);
    }
    if (!command) {
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            WITH s AS (
                SELECT id FROM %1$s WHERE id = $1 AND state IN ('TAKE'::%2$s, 'WORK'::%2$s) FOR UPDATE
            ) UPDATE %1$s AS u SET state = 'WORK'::%2$s, start = current_timestamp, pid = $2 FROM s WHERE u.id = s.id
            RETURNING input, COALESCE(EXTRACT(epoch FROM timeout), 0)::int4 * 1000 AS timeout, append, header, string, u.null, delimiter, quote, escape
        ), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_UPDATE_RETURNING, true);
    if (SPI_tuptable->numvals != 1) {
        W("%li: SPI_tuptable->numvals != 1", task->id);
        exit = true;
    } else {
        task->input = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "input", false));
        task->null = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "null", false));
        task->timeout = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "timeout", false));
        task->append = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "append", false));
        task->header = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "header", false));
        task->string = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "string", false));
        task->delimiter = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "delimiter", false));
        task->quote = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "quote", true));
        task->escape = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "escape", true));
        if (0 < StatementTimeout && StatementTimeout < task->timeout) task->timeout = StatementTimeout;
        D1("input = %s, timeout = %i, append = %s, header = %s, string = %s, null = %s, delimiter = %c, quote = %c, escape = %c", task->input, task->timeout, task->append ? "true" : "false", task->header ? "true" : "false", task->string ? "true" : "false", task->null, task->delimiter, task->quote, task->escape);
    }
    SPI_finish_my();
    return exit;
}

void task_delete(Task *task) {
    static Oid argtypes[] = {INT8OID};
    Datum values[] = {Int64GetDatum(task->id)};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        Work *work = task->work;
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(DELETE FROM %1$s WHERE id = $1 AND state IN ('DONE'::%2$s, 'FAIL'::%2$s)), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_DELETE, true);
    SPI_finish_my();
}

void task_error(Task *task, ErrorData *edata) {
    if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
    if (!task->error.data) initStringInfoMy(TopMemoryContext, &task->error);
    if (edata->elevel) appendStringInfo(&task->error, "%selevel%s%c%i", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, edata->elevel);
    if (edata->output_to_server) appendStringInfo(&task->error, "%soutput_to_server%s%ctrue", task->error.len ? "\n" : "", task->append ? "::bool" : "", task->delimiter);
    if (edata->output_to_client) appendStringInfo(&task->error, "%soutput_to_client%s%ctrue", task->error.len ? "\n" : "", task->append ? "::bool" : "", task->delimiter);
    if (edata->hide_stmt) appendStringInfo(&task->error, "%shide_stmt%s%ctrue", task->error.len ? "\n" : "", task->append ? "::bool" : "", task->delimiter);
    if (edata->hide_ctx) appendStringInfo(&task->error, "%shide_ctx%s%ctrue", task->error.len ? "\n" : "", task->append ? "::bool" : "", task->delimiter);
    if (edata->filename) appendStringInfo(&task->error, "%sfilename%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->filename);
    if (edata->lineno) appendStringInfo(&task->error, "%slineno%s%c%i", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, edata->lineno);
    if (edata->funcname) appendStringInfo(&task->error, "%sfuncname%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->funcname);
    if (edata->domain) appendStringInfo(&task->error, "%sdomain%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->domain);
    if (edata->context_domain) appendStringInfo(&task->error, "%scontext_domain%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->context_domain);
    if (edata->sqlerrcode) appendStringInfo(&task->error, "%ssqlerrcode%s%c%i", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, edata->sqlerrcode);
    if (edata->message) appendStringInfo(&task->error, "%smessage%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->message);
    if (edata->detail) appendStringInfo(&task->error, "%sdetail%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->detail);
    if (edata->detail_log) appendStringInfo(&task->error, "%sdetail_log%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->detail_log);
    if (edata->hint) appendStringInfo(&task->error, "%shint%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->hint);
    if (edata->context) appendStringInfo(&task->error, "%scontext%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->context);
    if (edata->message_id) appendStringInfo(&task->error, "%smessage_id%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->message_id);
    if (edata->schema_name) appendStringInfo(&task->error, "%sschema_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->schema_name);
    if (edata->table_name) appendStringInfo(&task->error, "%stable_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->table_name);
    if (edata->column_name) appendStringInfo(&task->error, "%scolumn_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->column_name);
    if (edata->datatype_name) appendStringInfo(&task->error, "%sdatatype_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->datatype_name);
    if (edata->constraint_name) appendStringInfo(&task->error, "%sconstraint_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->constraint_name);
    if (edata->cursorpos) appendStringInfo(&task->error, "%scursorpos%s%c%i", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, edata->cursorpos);
    if (edata->internalpos) appendStringInfo(&task->error, "%sinternalpos%s%c%i", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, edata->internalpos);
    if (edata->internalquery) appendStringInfo(&task->error, "%sinternalquery%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, edata->internalquery);
    if (edata->saved_errno) appendStringInfo(&task->error, "%ssaved_errno%s%c%i", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, edata->saved_errno);
    appendStringInfo(&task->output, SQL(%sROLLBACK), task->output.len ? "\n" : "");
    task->fail = true;
}

void task_repeat(Task *task) {
    static Oid argtypes[] = {INT8OID};
    Datum values[] = {Int64GetDatum(task->id)};
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        Work *work = task->work;
        StringInfoData buf;
        initStringInfoMy(TopMemoryContext, &buf);
        appendStringInfo(&buf, SQL(
            INSERT INTO %1$s (parent, dt, "group", max, input, timeout, delete, repeat, drift, count, live)
            SELECT $1, CASE
                WHEN drift THEN current_timestamp + repeat
                ELSE (WITH RECURSIVE s AS (SELECT dt AS t UNION SELECT t + repeat FROM s WHERE t <= current_timestamp) SELECT * FROM s ORDER BY 1 DESC LIMIT 1)
            END AS dt, t.group, max, input, timeout, delete, repeat, drift, count, live
            FROM %1$s AS t WHERE id = $1 AND state IN ('DONE'::%2$s, 'FAIL'::%2$s) LIMIT 1
        ), work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_INSERT, true);
    SPI_finish_my();
}

static void task_fail(Task *task) {
    MemoryContextData *oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
    ErrorData *edata = CopyErrorData();
    MemoryContextSwitchTo(oldMemoryContext);
    task_error(task, edata);
    FreeErrorData(edata);
    HOLD_INTERRUPTS();
    disable_all_timeouts(false);
    QueryCancelPending = false;
    EmitErrorReport();
    debug_query_string = NULL;
    AbortOutOfAnyTransaction();
    PortalErrorCleanup();
    SPICleanup();
    if (MyReplicationSlot) ReplicationSlotRelease();
    ReplicationSlotCleanup();
    jit_reset_after_error();
    MemoryContextSwitchTo(TopMemoryContext);
    FlushErrorState();
    xact_started = false;
    RESUME_INTERRUPTS();
}

static void SignalHandlerForShutdownRequestMy(SIGNAL_ARGS) {
    int save_errno = errno;
    ShutdownRequestPending = true;
    SetLatch(MyLatch);
    if (!DatumGetBool(DirectFunctionCall1(pg_cancel_backend, Int32GetDatum(MyProcPid)))) E("!pg_cancel_backend(%i)", MyProcPid);
    errno = save_errno;
}

static void task_init(Work *work, Task *task) {
    char *p = MyBgworkerEntry->bgw_extra;
    const char *schema_quote;
    const char *table_quote;
    MemoryContextData *oldcontext = CurrentMemoryContext;
    StringInfoData buf;
#define X(src, serialize, deserialize) deserialize(src);
    WORK
#undef X
    pqsignal(SIGTERM, SignalHandlerForShutdownRequestMy);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(work->conf.data, work->conf.user, 0);
    pgstat_report_appname(MyBgworkerEntry->bgw_type);
    process_session_preload_libraries();
    StartTransactionCommand();
    MemoryContextSwitchTo(oldcontext);
    work->conf.schema = get_namespace_name(work->schema);
    work->conf.table = get_rel_name(work->table);
    work->data = get_database_name(work->conf.data);
    work->user = GetUserNameFromId(work->conf.user, false);
    CommitTransactionCommand();
    MemoryContextSwitchTo(oldcontext);
    task->id = DatumGetInt64(MyBgworkerEntry->bgw_main_arg);
    task->work = work;
    if (!MyProcPort && !(MyProcPort = (Port *) calloc(1, sizeof(Port)))) E("!calloc");
    if (!MyProcPort->remote_host) MyProcPort->remote_host = "[local]";
    if (!MyProcPort->user_name) MyProcPort->user_name = work->user;
    if (!MyProcPort->database_name) MyProcPort->database_name = work->data;
    set_config_option("application_name", MyBgworkerEntry->bgw_type, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.data", work->data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.group", task->group, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    if (work->conf.schema) set_config_option("pg_task.schema", work->conf.schema, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.table", work->conf.table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.user", work->user, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    if (!MessageContext) MessageContext = AllocSetContextCreate(TopMemoryContext, "MessageContext", ALLOCSET_DEFAULT_SIZES);
    D1("user = %s, data = %s, schema = %s, table = %s, oid = %i, id = %li, group = %s, max = %i", work->user, work->data, work->conf.schema ? work->conf.schema : default_null, work->conf.table, work->table, task->id, task->group, task->max);
    schema_quote = work->conf.schema ? quote_identifier(work->conf.schema) : NULL;
    table_quote = quote_identifier(work->conf.table);
    initStringInfoMy(TopMemoryContext, &buf);
    if (work->conf.schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, table_quote);
    work->schema_table = buf.data;
    initStringInfoMy(TopMemoryContext, &buf);
    if (work->conf.schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, "state");
    work->schema_type = buf.data;
    initStringInfoMy(TopMemoryContext, &buf);
    appendStringInfo(&buf, "%i", work->table);
    set_config_option("pg_task.oid", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pfree(buf.data);
    if (work->conf.schema && schema_quote && work->conf.schema != schema_quote) pfree((void *)schema_quote);
    if (work->conf.table != table_quote) pfree((void *)table_quote);
    task->pid = MyProcPid;
    task->start = GetCurrentTimestamp();
    task->count = 0;
}

static void task_latch(void) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
}

static void task_success(Task *task) {
    MemoryContextData *oldMemoryContext = MemoryContextSwitchTo(MessageContext);
    MemoryContextResetAndDeleteChildren(MessageContext);
    InvalidateCatalogSnapshotConditionally();
    MemoryContextSwitchTo(oldMemoryContext);
    ReadyForQueryMy(task);
    SetCurrentStatementStartTimestamp();
    exec_simple_query_my(task);
    pfree(task->input);
    task->input = SQL(COMMIT);
    if (IsTransactionState()) exec_simple_query_my(task);
    if (IsTransactionState()) E("IsTransactionState");
}

static bool task_timeout(Task *task) {
    if (task_work(task)) return true;
    D1("id = %li, timeout = %i, input = %s, count = %i", task->id, task->timeout, task->input, task->count);
    PG_TRY();
        task_success(task);
    PG_CATCH();
        task_fail(task);
    PG_END_TRY();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);
    if (task_done(task)) return true;
    D1("repeat = %s, delete = %s, live = %s", task->repeat ? "true" : "false", task->delete ? "true" : "false", task->live ? "true" : "false");
    if (task->repeat) task_repeat(task);
    if (task->delete && !task->output.data) task_delete(task);
    if (task->output.data) pfree(task->output.data);
    task->output.data = NULL;
    if (task->error.data) pfree(task->error.data);
    task->error.data = NULL;
    if (ShutdownRequestPending) task->live = false;
    return !task->live || task_live(task);
}

void task(Datum main_arg) {
    Work *work = MemoryContextAllocZero(TopMemoryContext, sizeof(*work));
    Task *task = MemoryContextAllocZero(TopMemoryContext, sizeof(*task));
    task_init(work, task);
    while (!ShutdownRequestPending) {
        int rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 0, PG_WAIT_EXTENSION);
        if (rc & WL_TIMEOUT) if (task_timeout(task)) ShutdownRequestPending = true;
        if (rc & WL_LATCH_SET) task_latch();
        if (rc & WL_POSTMASTER_DEATH) ShutdownRequestPending = true;
    }
    pfree(task);
    pfree(work);
}
