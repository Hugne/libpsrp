/* Keyboard, buffer and credential types ([MS-PSRP] 2.2.3.25-30).
 *
 * These differ in which property bag they use, and it matters: KeyInfo uses
 * extended properties (<MS>) while BufferCell and PSCredential use adapted
 * ones (<Props>). Looking in the wrong bag finds nothing at all.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_host.h"

const char *psrp_buffer_cell_type_name(int32_t type)
{
    switch (type) {
    case PSRP_BUFFER_CELL_COMPLETE: return "Complete";
    case PSRP_BUFFER_CELL_LEADING:  return "Leading";
    case PSRP_BUFFER_CELL_TRAILING: return "Trailing";
    default:                        return "Unknown";
    }
}

const char *psrp_command_origin_name(int32_t origin)
{
    switch (origin) {
    case PSRP_COMMAND_ORIGIN_RUNSPACE: return "Runspace";
    case PSRP_COMMAND_ORIGIN_INTERNAL: return "Internal";
    default:                           return "Unknown";
    }
}

/* ------------------------------------------------------------ helpers -- */

typedef psrp_result_t (*add_fn)(psrp_object_t *, const char *, psrp_value_t *);

static psrp_result_t add_i32(psrp_object_t *o, const char *name, int32_t x,
                             add_fn add)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_int32(&v, x);
    rc = add(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static psrp_result_t add_char(psrp_object_t *o, const char *name, uint16_t ch,
                              add_fn add)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_char(&v, ch);
    rc = add(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static psrp_result_t add_bool(psrp_object_t *o, const char *name, bool b,
                              add_fn add)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_bool(&v, b);
    rc = add(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static int32_t get_i32(const psrp_object_t *o, const char *name, int32_t missing)
{
    const psrp_value_t *v = psrp_object_find(o, name);
    if (v && v->kind == PSRP_VAL_INT32) return v->as.i32;
    return missing;
}

static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* --------------------------------------------------------- 2.2.3.26 ---- */

psrp_result_t psrp_host_make_key_info(const psrp_key_info_t *k,
                                      psrp_value_t *out)
{
    psrp_object_t *o;
    psrp_result_t rc;

    if (!k || !out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);

    /* Extended properties, per 2.2.3.26. */
    rc = add_i32(o, "virtualKeyCode", k->virtual_key_code,
                 psrp_object_add_extended);
    if (rc == PSRP_OK)
        rc = add_char(o, "character", k->character, psrp_object_add_extended);
    if (rc == PSRP_OK)
        rc = add_i32(o, "controlKeyState", k->control_key_state,
                     psrp_object_add_extended);
    if (rc == PSRP_OK)
        rc = add_bool(o, "keyDown", k->key_down, psrp_object_add_extended);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    rc = psrp_value_set_object(out, o);
    if (rc != PSRP_OK) psrp_object_free(o);
    return rc;
}

psrp_result_t psrp_host_read_key_info(const psrp_value_t *v,
                                      psrp_key_info_t *out)
{
    const psrp_value_t *ch, *down;

    if (!v || !out || v->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;
    memset(out, 0, sizeof *out);

    ch = psrp_object_find(v->as.obj, "character");
    if (!ch || ch->kind != PSRP_VAL_CHAR) return PSRP_ERR_MALFORMED;
    out->character = ch->as.ch;

    out->virtual_key_code = get_i32(v->as.obj, "virtualKeyCode", 0);
    out->control_key_state = get_i32(v->as.obj, "controlKeyState", 0);

    down = psrp_object_find(v->as.obj, "keyDown");
    out->key_down = down && down->kind == PSRP_VAL_BOOL && down->as.b;
    return PSRP_OK;
}

/* --------------------------------------------------------- 2.2.3.28 ---- */

psrp_result_t psrp_host_make_buffer_cell(const psrp_buffer_cell_t *c,
                                         psrp_value_t *out)
{
    psrp_object_t *o;
    psrp_value_t color;
    psrp_result_t rc;

    if (!c || !out) return PSRP_ERR_INVALID_ARG;
    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);

    /* Adapted properties, per 2.2.3.28. */
    rc = add_char(o, "character", c->character, psrp_object_add_adapted);

    if (rc == PSRP_OK) {
        psrp_value_init(&color);
        rc = psrp_host_make_color(c->foreground_color, &color);
        if (rc == PSRP_OK)
            rc = psrp_object_add_adapted(o, "foregroundColor", &color);
        psrp_value_free(&color);
    }
    if (rc == PSRP_OK) {
        psrp_value_init(&color);
        rc = psrp_host_make_color(c->background_color, &color);
        if (rc == PSRP_OK)
            rc = psrp_object_add_adapted(o, "backgroundColor", &color);
        psrp_value_free(&color);
    }
    if (rc == PSRP_OK)
        rc = add_i32(o, "bufferCellType", c->cell_type,
                     psrp_object_add_adapted);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    rc = psrp_value_set_object(out, o);
    if (rc != PSRP_OK) psrp_object_free(o);
    return rc;
}

psrp_result_t psrp_host_read_buffer_cell(const psrp_value_t *v,
                                         psrp_buffer_cell_t *out)
{
    const psrp_value_t *ch, *fg, *bg;
    psrp_result_t rc;

    if (!v || !out || v->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;
    memset(out, 0, sizeof *out);

    ch = psrp_object_find(v->as.obj, "character");
    if (!ch || ch->kind != PSRP_VAL_CHAR) return PSRP_ERR_MALFORMED;
    out->character = ch->as.ch;

    /* The colours are Color wrappers, not bare ints. */
    fg = psrp_object_find(v->as.obj, "foregroundColor");
    bg = psrp_object_find(v->as.obj, "backgroundColor");
    if (!fg || !bg) return PSRP_ERR_MALFORMED;
    rc = psrp_host_read_color(fg, &out->foreground_color);
    if (rc != PSRP_OK) return rc;
    rc = psrp_host_read_color(bg, &out->background_color);
    if (rc != PSRP_OK) return rc;

    out->cell_type = get_i32(v->as.obj, "bufferCellType",
                             PSRP_BUFFER_CELL_COMPLETE);
    return PSRP_OK;
}

/* --------------------------------------------------------- 2.2.3.25 ---- */

psrp_result_t psrp_host_make_credential(const char *username,
                                        const char *password_ciphertext_b64,
                                        psrp_value_t *out)
{
    psrp_object_t *o;
    psrp_value_t v;
    psrp_result_t rc;

    if (!username || !password_ciphertext_b64 || !out)
        return PSRP_ERR_INVALID_ARG;

    o = psrp_object_new();
    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);
    psrp_object_set_type_ref_id(o, 0);

    /* 2.2.3.25 says MUST have these type names, not SHOULD. */
    rc = psrp_object_add_type_name(o,
        "System.Management.Automation.PSCredential");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(o, "System.Object");

    psrp_value_init(&v);
    if (rc == PSRP_OK) {
        rc = psrp_value_set_text(&v, PSRP_VAL_STRING, username,
                                 strlen(username));
        if (rc == PSRP_OK) rc = psrp_object_add_adapted(o, "UserName", &v);
        psrp_value_free(&v);
    }
    if (rc == PSRP_OK) {
        /* The password rides as a SecureString: base64 ciphertext under the
         * session key, which the caller must already have exchanged. */
        rc = psrp_value_set_text(&v, PSRP_VAL_SECURESTRING,
                                 password_ciphertext_b64,
                                 strlen(password_ciphertext_b64));
        if (rc == PSRP_OK) rc = psrp_object_add_adapted(o, "Password", &v);
        psrp_value_free(&v);
    }
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    rc = psrp_value_set_object(out, o);
    if (rc != PSRP_OK) psrp_object_free(o);
    return rc;
}

psrp_result_t psrp_host_read_credential(const psrp_value_t *v, char **username,
                                        char **password_ciphertext_b64)
{
    const psrp_value_t *u, *p;

    if (username) *username = NULL;
    if (password_ciphertext_b64) *password_ciphertext_b64 = NULL;
    if (!v || v->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;

    u = psrp_object_find(v->as.obj, "UserName");
    p = psrp_object_find(v->as.obj, "Password");
    if (!u || u->kind != PSRP_VAL_STRING) return PSRP_ERR_MALFORMED;
    if (!p || p->kind != PSRP_VAL_SECURESTRING) return PSRP_ERR_MALFORMED;

    if (username) {
        *username = dup_n(u->as.text.ptr, u->as.text.len);
        if (!*username) return PSRP_ERR_NOMEM;
    }
    if (password_ciphertext_b64) {
        *password_ciphertext_b64 = dup_n(p->as.text.ptr, p->as.text.len);
        if (!*password_ciphertext_b64) {
            if (username) { free(*username); *username = NULL; }
            return PSRP_ERR_NOMEM;
        }
    }
    return PSRP_OK;
}
