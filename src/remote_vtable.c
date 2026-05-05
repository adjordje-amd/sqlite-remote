#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLS 32
#define MAX_LINE 16384
#define MAX_ROWS 131072

typedef struct {
    sqlite3_vtab      base;
    char             *host;
    char             *db_path;
    char             *password;
    char             *table_name;
    char             *raw_sql;
    int               is_raw_query;
    int               num_cols;
    char             *col_names[MAX_COLS];
    int               row_count;
    int               data_cols;
    char             *rows[MAX_ROWS][MAX_COLS];
    sqlite3_int64     remote_rowids[MAX_ROWS];
} remote_vtab;

typedef struct {
    sqlite3_vtab_cursor base;
    int                 current_row;
} remote_cursor;

/* SSH ControlMaster opts: first call sets up a TCP master, subsequent
 * calls reuse it (no fresh handshake). Cuts per-call latency from
 * ~370ms to ~10ms after the master is up. ControlPath is keyed by
 * connection 4-tuple (%C is a hash of host:port:user:lhost). */
#define SSH_MUX_OPTS \
    "-o ControlMaster=auto " \
    "-o ControlPath=~/.ssh/cm-%%C " \
    "-o ControlPersist=10m"

static char *build_ssh_cmd(remote_vtab *v, const char *remote_sql)
{
    if (v->password && v->password[0])
        return sqlite3_mprintf(
            "sshpass -p '%q' ssh " SSH_MUX_OPTS " %s "
            "\"sqlite3 -separator $'\\t' %s '%q'\" 2>&1",
            v->password, v->host, v->db_path, remote_sql);

    return sqlite3_mprintf(
        "ssh " SSH_MUX_OPTS " %s "
        "\"sqlite3 -separator $'\\t' %s '%q'\" 2>&1",
        v->host, v->db_path, remote_sql);
}

static int run_remote_sql(remote_vtab *v, const char *sql, char **errmsg)
{
    char *cmd = build_ssh_cmd(v, sql);
    if (!cmd) return SQLITE_NOMEM;

    FILE *fp = popen(cmd, "r");
    sqlite3_free(cmd);
    if (!fp) {
        *errmsg = sqlite3_mprintf("failed to run ssh command");
        return SQLITE_ERROR;
    }

    char line[MAX_LINE];
    char output[MAX_LINE] = "";
    size_t total = 0;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (total + len < sizeof(output) - 1) {
            memcpy(output + total, line, len);
            total += len;
        }
    }
    output[total] = '\0';

    int status = pclose(fp);
    if (status != 0) {
        *errmsg = sqlite3_mprintf("%s", output[0] ? output : "ssh command failed");
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

static int query_remote_schema(remote_vtab *v, char **errmsg)
{
    char *pragma = sqlite3_mprintf("PRAGMA table_info(%s)", v->table_name);
    char *cmd    = build_ssh_cmd(v, pragma);
    sqlite3_free(pragma);
    if (!cmd) return SQLITE_NOMEM;

    FILE *fp = popen(cmd, "r");
    sqlite3_free(cmd);
    if (!fp) {
        *errmsg = sqlite3_mprintf("failed to query remote schema");
        return SQLITE_ERROR;
    }

    char line[MAX_LINE];
    v->num_cols = 0;
    while (fgets(line, sizeof(line), fp) && v->num_cols < MAX_COLS) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        char *saveptr = NULL;
        strtok_r(line, "\t", &saveptr);
        char *name = strtok_r(NULL, "\t", &saveptr);
        if (name)
            v->col_names[v->num_cols++] = sqlite3_mprintf("%s", name);
    }

    int status = pclose(fp);
    if (v->num_cols == 0) {
        *errmsg = sqlite3_mprintf(
            "no columns found for remote table '%s' (exit %d)",
            v->table_name, status);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

static int query_remote_schema_raw(remote_vtab *v, char **errmsg)
{
    char *wrapped = sqlite3_mprintf("SELECT * FROM (%s) LIMIT 1", v->raw_sql);
    if (!wrapped) return SQLITE_NOMEM;

    char *cmd;
    if (v->password && v->password[0])
        cmd = sqlite3_mprintf(
            "sshpass -p '%q' ssh " SSH_MUX_OPTS " %s "
            "\"sqlite3 -header -separator $'\\t' %s '%q'\" 2>&1",
            v->password, v->host, v->db_path, wrapped);
    else
        cmd = sqlite3_mprintf(
            "ssh " SSH_MUX_OPTS " %s "
            "\"sqlite3 -header -separator $'\\t' %s '%q'\" 2>&1",
            v->host, v->db_path, wrapped);
    sqlite3_free(wrapped);
    if (!cmd) return SQLITE_NOMEM;

    FILE *fp = popen(cmd, "r");
    sqlite3_free(cmd);
    if (!fp) {
        *errmsg = sqlite3_mprintf("failed to query remote schema for raw query");
        return SQLITE_ERROR;
    }

    char line[MAX_LINE];
    v->num_cols = 0;
    if (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        char *saveptr = NULL;
        char *tok = strtok_r(line, "\t", &saveptr);
        while (tok && v->num_cols < MAX_COLS) {
            v->col_names[v->num_cols++] = sqlite3_mprintf("%s", tok);
            tok = strtok_r(NULL, "\t", &saveptr);
        }
    }

    int status = pclose(fp);
    if (v->num_cols == 0) {
        *errmsg = sqlite3_mprintf(
            "no columns returned by raw query (exit %d)", status);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

static void vtab_free_rows(remote_vtab *v)
{
    for (int r = 0; r < v->row_count; r++)
        for (int c = 0; c < v->data_cols; c++)
            sqlite3_free(v->rows[r][c]);
    v->row_count = 0;
    v->data_cols = 0;
}

static int remote_connect(sqlite3 *db, void *aux, int argc,
                          const char *const *argv, sqlite3_vtab **vtab,
                          char **errmsg)
{
    (void)aux;

    if (argc < 5) {
        *errmsg = sqlite3_mprintf(
            "usage: CREATE VIRTUAL TABLE <name> USING "
            "remote_query(host, db_path [, password [, remote_table]])");
        return SQLITE_ERROR;
    }

    remote_vtab *v = sqlite3_malloc(sizeof(*v));
    if (!v) return SQLITE_NOMEM;
    memset(v, 0, sizeof(*v));

    v->host     = sqlite3_mprintf("%s", argv[3]);
    v->db_path  = sqlite3_mprintf("%s", argv[4]);
    v->password = (argc > 5) ? sqlite3_mprintf("%s", argv[5]) : NULL;

    const char *fourth_arg = (argc > 6) ? argv[6] : argv[2];

    /* strip surrounding quotes from arguments */
    char **fields[] = { &v->host, &v->db_path, &v->password };
    for (int i = 0; i < 3; i++) {
        if (!fields[i] || !*fields[i]) continue;
        char *s = *fields[i];
        size_t len = strlen(s);
        if (len >= 2 && ((s[0] == '"' && s[len-1] == '"') ||
                         (s[0] == '\'' && s[len-1] == '\''))) {
            memmove(s, s + 1, len - 2);
            s[len - 2] = '\0';
        }
    }

    /* detect raw query mode: 4th arg starts with SELECT (case-insensitive) */
    char *unquoted_fourth = sqlite3_mprintf("%s", fourth_arg);
    if (unquoted_fourth) {
        size_t len = strlen(unquoted_fourth);
        if (len >= 2 && ((unquoted_fourth[0] == '"' && unquoted_fourth[len-1] == '"') ||
                         (unquoted_fourth[0] == '\'' && unquoted_fourth[len-1] == '\''))) {
            memmove(unquoted_fourth, unquoted_fourth + 1, len - 2);
            unquoted_fourth[len - 2] = '\0';
        }
    }

    int rc;
    if (unquoted_fourth &&
        (strncmp(unquoted_fourth, "SELECT", 6) == 0 ||
         strncmp(unquoted_fourth, "select", 6) == 0)) {
        v->is_raw_query = 1;
        v->raw_sql      = unquoted_fourth;
        v->table_name   = NULL;
        rc = query_remote_schema_raw(v, errmsg);
    } else {
        v->is_raw_query = 0;
        v->raw_sql      = NULL;
        v->table_name   = unquoted_fourth;
        rc = query_remote_schema(v, errmsg);
    }
    if (rc != SQLITE_OK) goto fail;

    {
        char *ddl = sqlite3_mprintf("CREATE TABLE x(");
        for (int i = 0; i < v->num_cols; i++) {
            char *tmp = (i == 0)
                ? sqlite3_mprintf("%s%s TEXT", ddl, v->col_names[i])
                : sqlite3_mprintf("%s, %s TEXT", ddl, v->col_names[i]);
            sqlite3_free(ddl);
            ddl = tmp;
        }
        char *full = sqlite3_mprintf("%s)", ddl);
        sqlite3_free(ddl);
        rc = sqlite3_declare_vtab(db, full);
        sqlite3_free(full);
        if (rc != SQLITE_OK) goto fail;
    }

    *vtab = &v->base;
    return SQLITE_OK;

fail:
    for (int i = 0; i < v->num_cols; i++)
        sqlite3_free(v->col_names[i]);
    sqlite3_free(v->table_name);
    sqlite3_free(v->raw_sql);
    sqlite3_free(v->host);
    sqlite3_free(v->db_path);
    sqlite3_free(v->password);
    sqlite3_free(v);
    return rc;
}

static int remote_disconnect(sqlite3_vtab *vtab)
{
    remote_vtab *v = (remote_vtab *)vtab;
    vtab_free_rows(v);
    for (int i = 0; i < v->num_cols; i++)
        sqlite3_free(v->col_names[i]);
    sqlite3_free(v->table_name);
    sqlite3_free(v->raw_sql);
    sqlite3_free(v->host);
    sqlite3_free(v->db_path);
    sqlite3_free(v->password);
    sqlite3_free(v);
    return SQLITE_OK;
}

static int remote_best_index(sqlite3_vtab *vtab, sqlite3_index_info *info)
{
    (void)vtab;
    info->estimatedCost = 1000000.0;
    return SQLITE_OK;
}

static int remote_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **cursor)
{
    (void)vtab;
    remote_cursor *c = sqlite3_malloc(sizeof(*c));
    if (!c) return SQLITE_NOMEM;
    memset(c, 0, sizeof(*c));
    *cursor = &c->base;
    return SQLITE_OK;
}

static int remote_close(sqlite3_vtab_cursor *cursor)
{
    sqlite3_free(cursor);
    return SQLITE_OK;
}

static int parse_row_raw(remote_vtab *v, const char *line, int row)
{
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    v->remote_rowids[row] = row;

    char *saveptr = NULL;
    char *tok = strtok_r(buf, "\t", &saveptr);
    int col = 0;
    while (tok && col < MAX_COLS) {
        v->rows[row][col] = sqlite3_mprintf("%s", tok);
        col++;
        tok = strtok_r(NULL, "\t", &saveptr);
    }
    return col;
}

static int parse_row(remote_vtab *v, const char *line, int row)
{
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    /* first field is remote rowid */
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "\t", &saveptr);
    if (!tok) return 0;
    v->remote_rowids[row] = strtoll(tok, NULL, 10);

    int col = 0;
    tok = strtok_r(NULL, "\t", &saveptr);
    while (tok && col < MAX_COLS) {
        v->rows[row][col] = sqlite3_mprintf("%s", tok);
        col++;
        tok = strtok_r(NULL, "\t", &saveptr);
    }
    return col;
}

static int remote_filter(sqlite3_vtab_cursor *cursor, int idx_num,
                         const char *idx_str, int argc,
                         sqlite3_value **argv)
{
    (void)idx_num;
    (void)idx_str;
    (void)argc;
    (void)argv;

    remote_cursor *cur = (remote_cursor *)cursor;
    remote_vtab   *v   = (remote_vtab *)cursor->pVtab;

    vtab_free_rows(v);

    char *sql;
    if (v->is_raw_query)
        sql = sqlite3_mprintf("%s", v->raw_sql);
    else
        sql = sqlite3_mprintf("SELECT rowid, * FROM %s", v->table_name);

    char *cmd = build_ssh_cmd(v, sql);
    sqlite3_free(sql);
    if (!cmd) return SQLITE_NOMEM;

    FILE *fp = popen(cmd, "r");
    sqlite3_free(cmd);
    if (!fp) {
        v->base.zErrMsg = sqlite3_mprintf("failed to run ssh command");
        return SQLITE_ERROR;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) && v->row_count < MAX_ROWS) {
        int cols = v->is_raw_query
            ? parse_row_raw(v, line, v->row_count)
            : parse_row(v, line, v->row_count);
        if (v->row_count == 0)
            v->data_cols = cols;
        v->row_count++;
    }

    int status = pclose(fp);
    if (status != 0 && v->row_count == 0) {
        v->base.zErrMsg = sqlite3_mprintf("ssh command failed (exit %d)",
                                          status);
        return SQLITE_ERROR;
    }

    cur->current_row = 0;
    return SQLITE_OK;
}

static int remote_eof(sqlite3_vtab_cursor *cursor)
{
    remote_cursor *cur = (remote_cursor *)cursor;
    remote_vtab   *v   = (remote_vtab *)cursor->pVtab;
    return cur->current_row >= v->row_count;
}

static int remote_next(sqlite3_vtab_cursor *cursor)
{
    remote_cursor *cur = (remote_cursor *)cursor;
    cur->current_row++;
    return SQLITE_OK;
}

static int remote_column(sqlite3_vtab_cursor *cursor,
                         sqlite3_context *ctx, int col)
{
    remote_cursor *cur = (remote_cursor *)cursor;
    remote_vtab   *v   = (remote_vtab *)cursor->pVtab;

    if (col < v->data_cols)
        sqlite3_result_text(ctx, v->rows[cur->current_row][col], -1,
                            SQLITE_TRANSIENT);
    else
        sqlite3_result_null(ctx);

    return SQLITE_OK;
}

static int remote_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *rowid)
{
    remote_cursor *cur = (remote_cursor *)cursor;
    *rowid = cur->current_row;
    return SQLITE_OK;
}

static char *quote_value(sqlite3_value *val)
{
    switch (sqlite3_value_type(val)) {
    case SQLITE_NULL:    return sqlite3_mprintf("NULL");
    case SQLITE_INTEGER: return sqlite3_mprintf("%lld", sqlite3_value_int64(val));
    case SQLITE_FLOAT:   return sqlite3_mprintf("%f", sqlite3_value_double(val));
    default:             return sqlite3_mprintf("''%q''", sqlite3_value_text(val));
    }
}

static int remote_update(sqlite3_vtab *vtab, int argc,
                         sqlite3_value **argv, sqlite3_int64 *rowid)
{
    remote_vtab *v = (remote_vtab *)vtab;

    if (v->is_raw_query) {
        v->base.zErrMsg = sqlite3_mprintf(
            "raw query virtual tables are read-only");
        return SQLITE_READONLY;
    }

    char *sql = NULL;

    if (argc == 1) {
        /* DELETE: argv[0] = rowid */
        int row = (int)sqlite3_value_int64(argv[0]);
        if (row < 0 || row >= v->row_count) {
            v->base.zErrMsg = sqlite3_mprintf("invalid rowid");
            return SQLITE_ERROR;
        }
        sql = sqlite3_mprintf("DELETE FROM %s WHERE rowid = %lld",
                              v->table_name, v->remote_rowids[row]);

    } else if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        /* INSERT: argv[2..] = column values */
        char *cols = sqlite3_mprintf("");
        char *vals = sqlite3_mprintf("");
        for (int i = 0; i < v->num_cols; i++) {
            char *qv = quote_value(argv[2 + i]);
            char *tc = (i == 0)
                ? sqlite3_mprintf("%s%s", cols, v->col_names[i])
                : sqlite3_mprintf("%s, %s", cols, v->col_names[i]);
            char *tv = (i == 0)
                ? sqlite3_mprintf("%s%s", vals, qv)
                : sqlite3_mprintf("%s, %s", vals, qv);
            sqlite3_free(cols); cols = tc;
            sqlite3_free(vals); vals = tv;
            sqlite3_free(qv);
        }
        sql = sqlite3_mprintf("INSERT INTO %s(%s) VALUES(%s)",
                              v->table_name, cols, vals);
        sqlite3_free(cols);
        sqlite3_free(vals);
        (void)rowid;

    } else {
        /* UPDATE: argv[0] = old rowid, argv[2..] = new values */
        int row = (int)sqlite3_value_int64(argv[0]);
        if (row < 0 || row >= v->row_count) {
            v->base.zErrMsg = sqlite3_mprintf("invalid rowid");
            return SQLITE_ERROR;
        }
        char *sets = sqlite3_mprintf("");
        for (int i = 0; i < v->num_cols; i++) {
            char *qv = quote_value(argv[2 + i]);
            char *tmp = (i == 0)
                ? sqlite3_mprintf("%s%s = %s", sets, v->col_names[i], qv)
                : sqlite3_mprintf("%s, %s = %s", sets, v->col_names[i], qv);
            sqlite3_free(sets); sets = tmp;
            sqlite3_free(qv);
        }
        sql = sqlite3_mprintf("UPDATE %s SET %s WHERE rowid = %lld",
                              v->table_name, sets, v->remote_rowids[row]);
        sqlite3_free(sets);
    }

    if (!sql) return SQLITE_NOMEM;

    char *errmsg = NULL;
    int rc = run_remote_sql(v, sql, &errmsg);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) {
        v->base.zErrMsg = errmsg;
        return rc;
    }
    return SQLITE_OK;
}

static sqlite3_module remote_module = {
    .iVersion    = 1,
    .xCreate     = remote_connect,
    .xConnect    = remote_connect,
    .xBestIndex  = remote_best_index,
    .xDisconnect = remote_disconnect,
    .xDestroy    = remote_disconnect,
    .xOpen       = remote_open,
    .xClose      = remote_close,
    .xFilter     = remote_filter,
    .xNext       = remote_next,
    .xEof        = remote_eof,
    .xColumn     = remote_column,
    .xRowid      = remote_rowid,
    .xUpdate     = remote_update,
};

static void remote_connect_db_func(sqlite3_context *ctx, int argc,
                                   sqlite3_value **argv)
{
    if (argc < 3) {
        sqlite3_result_error(ctx,
            "usage: remote_connect_db(host, db_path, password)", -1);
        return;
    }

    const char *host = (const char *)sqlite3_value_text(argv[0]);
    const char *db_path = (const char *)sqlite3_value_text(argv[1]);
    const char *pass = (const char *)sqlite3_value_text(argv[2]);
    if (!host || !db_path) {
        sqlite3_result_error(ctx, "host and db_path must not be NULL", -1);
        return;
    }

    /* query remote for list of tables */
    char *cmd;
    if (pass && pass[0])
        cmd = sqlite3_mprintf(
            "sshpass -p %s ssh %s \"sqlite3 %s "
            "\\\"SELECT name FROM sqlite_master WHERE type='table'\\\"\" 2>&1",
            pass, host, db_path);
    else
        cmd = sqlite3_mprintf(
            "ssh %s \"sqlite3 %s "
            "\\\"SELECT name FROM sqlite_master WHERE type='table'\\\"\" 2>&1",
            host, db_path);

    if (!cmd) { sqlite3_result_error_nomem(ctx); return; }

    FILE *fp = popen(cmd, "r");
    sqlite3_free(cmd);
    if (!fp) {
        sqlite3_result_error(ctx, "failed to run ssh command", -1);
        return;
    }

    char tables[256][128];
    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && count < 256) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0])
            strncpy(tables[count++], line, 127);
    }

    int status = pclose(fp);
    if (status != 0 || count == 0) {
        sqlite3_result_error(ctx, "failed to list remote tables", -1);
        return;
    }

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    int created = 0;
    char *result = sqlite3_mprintf("");

    for (int i = 0; i < count; i++) {
        char *sql;
        if (pass && pass[0])
            sql = sqlite3_mprintf(
                "CREATE VIRTUAL TABLE [%s] USING remote_query(%Q, %Q, %Q)",
                tables[i], host, db_path, pass);
        else
            sql = sqlite3_mprintf(
                "CREATE VIRTUAL TABLE [%s] USING remote_query(%Q, %Q)",
                tables[i], host, db_path);

        char *err = NULL;
        int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
        sqlite3_free(sql);

        char *tmp;
        if (rc == SQLITE_OK) {
            tmp = sqlite3_mprintf("%s%s%s", result,
                                  created ? ", " : "", tables[i]);
            created++;
        } else {
            tmp = sqlite3_mprintf("%s%s%s (skipped)", result,
                                  created ? ", " : "", tables[i]);
            sqlite3_free(err);
        }
        sqlite3_free(result);
        result = tmp;
    }

    char *msg = sqlite3_mprintf("Connected %d tables: %s", created, result);
    sqlite3_result_text(ctx, msg, -1, sqlite3_free);
    sqlite3_free(result);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_remote_init(sqlite3 *db, char **errmsg,
                        const sqlite3_api_routines *api)
{
    (void)errmsg;
    SQLITE_EXTENSION_INIT2(api);

    int rc = sqlite3_create_module(db, "remote_query", &remote_module, NULL);
    if (rc != SQLITE_OK) return rc;

    return sqlite3_create_function(db, "remote_connect_db", 3,
                                   SQLITE_UTF8, NULL,
                                   remote_connect_db_func, NULL, NULL);
}
