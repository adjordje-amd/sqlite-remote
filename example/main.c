#include <sqlite3.h>
#include <stdio.h>

static void print_results(sqlite3 *conn, const char *sql)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(conn));
        return;
    }

    int col_count = sqlite3_column_count(stmt);
    for (int i = 0; i < col_count; i++) {
        if (i > 0) printf("\t");
        printf("%s", sqlite3_column_name(stmt, i));
    }
    printf("\n");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        for (int i = 0; i < col_count; i++) {
            const char *val = (const char *)sqlite3_column_text(stmt, i);
            if (i > 0) printf("\t");
            printf("%s", val ? val : "NULL");
        }
        printf("\n");
    }
    sqlite3_finalize(stmt);
}

static int exec(sqlite3 *conn, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(conn, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err);
        sqlite3_free(err);
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *host = (argc > 1) ? argv[1] : "amd@RSN-SWSLAB-03-L";
    const char *db   = (argc > 2) ? argv[2] : "/tmp/test.db";
    const char *pass = (argc > 3) ? argv[3] : "amd123";

    sqlite3 *conn;
    if (sqlite3_open(":memory:", &conn) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open: %s\n", sqlite3_errmsg(conn));
        return 1;
    }

    sqlite3_enable_load_extension(conn, 1);
    if (sqlite3_load_extension(conn, "./build/remote", NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "load_extension: %s\n", sqlite3_errmsg(conn));
        sqlite3_close(conn);
        return 1;
    }

    char *sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE t USING remote_query(%Q, %Q, %Q)",
        host, db, pass);
    if (exec(conn, sql)) { sqlite3_free(sql); sqlite3_close(conn); return 1; }
    sqlite3_free(sql);

    exec(conn, "INSERT INTO t VALUES(1, 'alice', 30)");
    exec(conn, "INSERT INTO t VALUES(2, 'bob', 25)");
    exec(conn, "INSERT INTO t VALUES(3, 'carol', 35)");

    printf("-- After INSERT:\n");
    print_results(conn, "SELECT * FROM t");

    exec(conn, "UPDATE t SET age = 31 WHERE name = 'alice'");
    exec(conn, "DELETE FROM t WHERE id = 2");

    printf("\n-- After UPDATE + DELETE:\n");
    print_results(conn, "SELECT * FROM t");

    sqlite3_close(conn);
    return 0;
}
