/* Host value types and _hostDefaultData ([MS-PSRP] 2.2.3.1-3, 2.2.3.14).
 *
 * Coordinates, Size and Color share one shape: an object carrying a T
 * property naming the .NET type and a V property holding the value. Colour's
 * V is a plain int; the other two nest a second object.
 */

#include <stdlib.h>
#include <string.h>

#include "psrp/psrp_host.h"

const char *psrp_console_color_name(int32_t color)
{
    switch (color) {
    /* 0 is absent from the spec's table but is Black in System.ConsoleColor,
     * which is the type the T property names. */
    case PSRP_COLOR_BLACK:        return "Black";
    case PSRP_COLOR_DARK_BLUE:    return "DarkBlue";
    case PSRP_COLOR_DARK_GREEN:   return "DarkGreen";
    case PSRP_COLOR_DARK_CYAN:    return "DarkCyan";
    case PSRP_COLOR_DARK_RED:     return "DarkRed";
    case PSRP_COLOR_DARK_MAGENTA: return "DarkMagenta";
    case PSRP_COLOR_DARK_YELLOW:  return "DarkYellow";
    case PSRP_COLOR_GRAY:         return "Gray";
    case PSRP_COLOR_DARK_GRAY:    return "DarkGray";
    case PSRP_COLOR_BLUE:         return "Blue";
    case PSRP_COLOR_GREEN:        return "Green";
    case PSRP_COLOR_CYAN:         return "Cyan";
    case PSRP_COLOR_RED:          return "Red";
    case PSRP_COLOR_MAGENTA:      return "Magenta";
    case PSRP_COLOR_YELLOW:       return "Yellow";
    case PSRP_COLOR_WHITE:        return "White";
    default:                      return "Unknown";
    }
}

/* ------------------------------------------------------------ helpers -- */

static psrp_result_t add_i32(psrp_object_t *o, const char *name, int32_t x)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    psrp_value_set_int32(&v, x);
    rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

static psrp_result_t add_str(psrp_object_t *o, const char *name, const char *s)
{
    psrp_value_t v;
    psrp_result_t rc;
    psrp_value_init(&v);
    rc = psrp_value_set_text(&v, PSRP_VAL_STRING, s, strlen(s));
    if (rc == PSRP_OK) rc = psrp_object_add_extended(o, name, &v);
    psrp_value_free(&v);
    return rc;
}

/* Wraps `inner` (already a value) as { T: type_name, V: inner }. */
static psrp_result_t make_typed(const char *type_name, psrp_value_t *inner,
                                psrp_value_t *out)
{
    psrp_object_t *o = psrp_object_new();
    psrp_result_t rc;

    if (!o) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(o, 0);
    rc = add_str(o, "T", type_name);
    if (rc == PSRP_OK) rc = psrp_object_add_extended(o, "V", inner);
    if (rc != PSRP_OK) { psrp_object_free(o); return rc; }

    rc = psrp_value_set_object(out, o);
    if (rc != PSRP_OK) psrp_object_free(o);
    return rc;
}

/* Returns the V property of a T/V wrapper, checking T when `expect` is set. */
static const psrp_value_t *typed_value(const psrp_value_t *v,
                                       const char *expect)
{
    const psrp_value_t *t, *inner;
    if (!v || v->kind != PSRP_VAL_OBJECT) return NULL;
    if (expect) {
        t = psrp_object_find(v->as.obj, "T");
        if (!t || t->kind != PSRP_VAL_STRING) return NULL;
        if (strcmp(t->as.text.ptr, expect) != 0) return NULL;
    }
    inner = psrp_object_find(v->as.obj, "V");
    return inner;
}

static psrp_result_t read_two_ints(const psrp_value_t *v, const char *type_name,
                                   const char *first, const char *second,
                                   int32_t *a, int32_t *b)
{
    const psrp_value_t *inner = typed_value(v, type_name);
    const psrp_value_t *pa, *pb;

    if (!inner || inner->kind != PSRP_VAL_OBJECT) return PSRP_ERR_MALFORMED;
    pa = psrp_object_find(inner->as.obj, first);
    pb = psrp_object_find(inner->as.obj, second);
    if (!pa || pa->kind != PSRP_VAL_INT32) return PSRP_ERR_MALFORMED;
    if (!pb || pb->kind != PSRP_VAL_INT32) return PSRP_ERR_MALFORMED;
    if (a) *a = pa->as.i32;
    if (b) *b = pb->as.i32;
    return PSRP_OK;
}

/* -------------------------------------------------------- constructors -- */

psrp_result_t psrp_host_make_coordinates(int32_t x, int32_t y, psrp_value_t *out)
{
    psrp_object_t *inner;
    psrp_value_t iv;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    inner = psrp_object_new();
    if (!inner) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(inner, 0);
    rc = add_i32(inner, "x", x);
    if (rc == PSRP_OK) rc = add_i32(inner, "y", y);
    if (rc != PSRP_OK) { psrp_object_free(inner); return rc; }

    psrp_value_init(&iv);
    rc = psrp_value_set_object(&iv, inner);
    if (rc != PSRP_OK) { psrp_object_free(inner); return rc; }
    rc = make_typed("System.Management.Automation.Host.Coordinates", &iv, out);
    psrp_value_free(&iv);
    return rc;
}

psrp_result_t psrp_host_make_size(int32_t width, int32_t height,
                                  psrp_value_t *out)
{
    psrp_object_t *inner;
    psrp_value_t iv;
    psrp_result_t rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    inner = psrp_object_new();
    if (!inner) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(inner, 0);
    rc = add_i32(inner, "width", width);
    if (rc == PSRP_OK) rc = add_i32(inner, "height", height);
    if (rc != PSRP_OK) { psrp_object_free(inner); return rc; }

    psrp_value_init(&iv);
    rc = psrp_value_set_object(&iv, inner);
    if (rc != PSRP_OK) { psrp_object_free(inner); return rc; }
    rc = make_typed("System.Management.Automation.Host.Size", &iv, out);
    psrp_value_free(&iv);
    return rc;
}

psrp_result_t psrp_host_make_color(int32_t color, psrp_value_t *out)
{
    psrp_value_t iv;
    psrp_result_t rc;
    if (!out) return PSRP_ERR_INVALID_ARG;
    /* Colour's V is the int itself, not a nested object. */
    psrp_value_init(&iv);
    psrp_value_set_int32(&iv, color);
    rc = make_typed("System.ConsoleColor", &iv, out);
    psrp_value_free(&iv);
    return rc;
}

psrp_result_t psrp_host_make_string(const char *utf8, psrp_value_t *out)
{
    psrp_value_t iv;
    psrp_result_t rc;
    if (!out || !utf8) return PSRP_ERR_INVALID_ARG;
    psrp_value_init(&iv);
    rc = psrp_value_set_text(&iv, PSRP_VAL_STRING, utf8, strlen(utf8));
    if (rc == PSRP_OK) rc = make_typed("System.String", &iv, out);
    psrp_value_free(&iv);
    return rc;
}

/* ------------------------------------------------------------ readers -- */

psrp_result_t psrp_host_read_coordinates(const psrp_value_t *v, int32_t *x,
                                         int32_t *y)
{
    return read_two_ints(v, "System.Management.Automation.Host.Coordinates",
                         "x", "y", x, y);
}

psrp_result_t psrp_host_read_size(const psrp_value_t *v, int32_t *width,
                                  int32_t *height)
{
    return read_two_ints(v, "System.Management.Automation.Host.Size",
                         "width", "height", width, height);
}

psrp_result_t psrp_host_read_color(const psrp_value_t *v, int32_t *color)
{
    const psrp_value_t *inner = typed_value(v, "System.ConsoleColor");
    if (!inner || inner->kind != PSRP_VAL_INT32) return PSRP_ERR_MALFORMED;
    if (color) *color = inner->as.i32;
    return PSRP_OK;
}

/* ------------------------------------------------- _hostDefaultData ---- */

void psrp_host_default_data_defaults(psrp_host_default_data_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->foreground_color = PSRP_COLOR_GRAY;
    out->background_color = PSRP_COLOR_BLACK;
    out->cursor_size = 25;
    out->buffer_width = 120;
    out->buffer_height = 3000;
    out->window_width = 120;
    out->window_height = 50;
    out->max_window_width = 120;
    out->max_window_height = 50;
    out->max_physical_window_width = 240;
    out->max_physical_window_height = 100;
    out->window_title = "libpsrp";
}

/* Adds one dictionary entry: an I32 key and a typed-value wrapper. */
static psrp_result_t add_entry(psrp_object_t *dict, int32_t key,
                               psrp_value_t *value)
{
    psrp_value_t k;
    psrp_result_t rc;
    psrp_value_init(&k);
    psrp_value_set_int32(&k, key);
    rc = psrp_object_add_entry(dict, &k, value);
    psrp_value_free(&k);
    psrp_value_free(value);
    return rc;
}

psrp_result_t psrp_host_build_default_data(const psrp_host_default_data_t *d,
                                           psrp_value_t *out)
{
    psrp_object_t *wrapper, *dict;
    psrp_value_t v, dv;
    psrp_result_t rc;

    if (!d || !out) return PSRP_ERR_INVALID_ARG;

    dict = psrp_object_new();
    if (!dict) return PSRP_ERR_NOMEM;
    psrp_object_set_ref_id(dict, 0);
    psrp_object_set_type_ref_id(dict, 0);
    rc = psrp_object_add_type_name(dict, "System.Collections.Hashtable");
    if (rc == PSRP_OK) rc = psrp_object_add_type_name(dict, "System.Object");
    psrp_object_set_container(dict, PSRP_CONTAINER_DICT);
    if (rc != PSRP_OK) { psrp_object_free(dict); return rc; }

    psrp_value_init(&v);

    /* Keys are exactly the ten from the 2.2.3.14 table. */
    if (rc == PSRP_OK && (rc = psrp_host_make_color(d->foreground_color, &v)) == PSRP_OK)
        rc = add_entry(dict, 0, &v);
    if (rc == PSRP_OK && (rc = psrp_host_make_color(d->background_color, &v)) == PSRP_OK)
        rc = add_entry(dict, 1, &v);
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_coordinates(d->cursor_position_x,
                                         d->cursor_position_y, &v)) == PSRP_OK)
        rc = add_entry(dict, 2, &v);
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_coordinates(d->window_position_x,
                                         d->window_position_y, &v)) == PSRP_OK)
        rc = add_entry(dict, 3, &v);
    if (rc == PSRP_OK) {
        /* Key 4 is a bare Int32, not a Size. */
        psrp_value_t iv;
        psrp_value_init(&iv);
        psrp_value_set_int32(&iv, d->cursor_size);
        rc = make_typed("System.Int32", &iv, &v);
        psrp_value_free(&iv);
        if (rc == PSRP_OK) rc = add_entry(dict, 4, &v);
    }
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_size(d->buffer_width, d->buffer_height, &v)) == PSRP_OK)
        rc = add_entry(dict, 5, &v);
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_size(d->window_width, d->window_height, &v)) == PSRP_OK)
        rc = add_entry(dict, 6, &v);
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_size(d->max_window_width,
                                  d->max_window_height, &v)) == PSRP_OK)
        rc = add_entry(dict, 7, &v);
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_size(d->max_physical_window_width,
                                  d->max_physical_window_height, &v)) == PSRP_OK)
        rc = add_entry(dict, 8, &v);
    if (rc == PSRP_OK &&
        (rc = psrp_host_make_string(d->window_title ? d->window_title : "",
                                    &v)) == PSRP_OK)
        rc = add_entry(dict, 9, &v);

    psrp_value_free(&v);
    if (rc != PSRP_OK) { psrp_object_free(dict); return rc; }

    /* The dictionary hangs off a `data` property of the _hostDefaultData
     * object, per the 2.2.2.2 example. */
    wrapper = psrp_object_new();
    if (!wrapper) { psrp_object_free(dict); return PSRP_ERR_NOMEM; }
    psrp_object_set_ref_id(wrapper, 0);

    psrp_value_init(&dv);
    rc = psrp_value_set_object(&dv, dict);
    if (rc != PSRP_OK) {
        psrp_object_free(dict);
        psrp_object_free(wrapper);
        return rc;
    }
    rc = psrp_object_add_extended(wrapper, "data", &dv);
    psrp_value_free(&dv);
    if (rc != PSRP_OK) { psrp_object_free(wrapper); return rc; }

    rc = psrp_value_set_object(out, wrapper);
    if (rc != PSRP_OK) psrp_object_free(wrapper);
    return rc;
}
