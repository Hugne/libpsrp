/* The same job as run_command.c, using the convenience layer.
 *
 *   quick_run <connection> <user> <password> <command> [<command> ...]
 *   quick_run http://localhost:5985/wsman Administrator secret "Get-Date"
 *
 * run_command.c is the honest picture of the protocol: a session that does no
 * I/O, a transport that does, and a pump loop joining them. Read that one to
 * understand the library. Read this one to use it.
 *
 * Note that several commands share one connection. That is the substantive
 * difference, not the line count: each extra command here costs a pipeline,
 * where a fresh client per command would cost a whole remote shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_client.h"
#include "psrp/psrp_transport.h"

/* Prints one stream, if it has anything in it. */
static void print_stream(const char *label, const psrp_stream_t *st, FILE *to)
{
    size_t i;
    for (i = 0; i < st->count; i++)
        fprintf(to, "%s: %s\n", label, st->items[i]);
}

int main(int argc, char **argv)
{
    psrp_client_config_t cfg;
    psrp_client_t *c = NULL;
    const char *conn = NULL, *user = NULL, *pass = NULL;
    int status = 1;
    int i;
    psrp_result_t rc;

    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <connection> <user> <password> <command>...\n"
                "  e.g. %s http://localhost:5985/wsman Administrator pw "
                "\"Get-Date\" \"$PID\"\n",
                argv[0], argv[0]);
        return 2;
    }

    conn = argv[1];
    user = argv[2];
    pass = argv[3];

    memset(&cfg, 0, sizeof cfg);
    cfg.connection = conn;
    cfg.username = user;
    cfg.password = pass;

    rc = psrp_client_connect(&cfg, &c);
    if (rc != PSRP_OK) {
        /* The return code carries the diagnosis: refused credentials, an
         * endpoint that is not there and a protocol failure are three
         * different codes rather than one "connect failed" for all of them. */
        fprintf(stderr, "connect to %s failed: %s\n", conn, psrp_strerror(rc));
        goto done;
    }

    status = 0;
    for (i = 4; i < argc; i++) {
        psrp_run_result_t r;
        psrp_buffer_t text;

        if (psrp_client_run(c, argv[i], &r) != PSRP_OK) {
            fprintf(stderr, "%s: %s\n", argv[i], psrp_client_last_error(c));
            status = 1;
            continue;
        }

        psrp_buffer_init(&text);
        if (psrp_run_result_text(&r, &text) == PSRP_OK &&
            psrp_buffer_append_u8(&text, 0) == PSRP_OK)
            fputs((const char *)text.data, stdout);
        psrp_buffer_free(&text);

        /* Kept apart from the output on purpose: a caller that pipes stdout
         * somewhere still wants to see these. */
        print_stream("error", &r.errors, stderr);
        print_stream("warning", &r.warnings, stderr);

        if (r.state != PSRP_INVOCATION_COMPLETED) {
            fprintf(stderr, "[%s]\n", psrp_invocation_state_name(r.state));
            status = 1;
        }
        psrp_run_result_free(&r);
    }

done:
    psrp_client_free(c);
    return status;
}
