/* TimeZone ([MS-PSRP] 2.2.3.10) in MS-NRBF.
 *
 * The graph is small and completely fixed by 2.2.3.10.1, so this writes and
 * reads exactly that shape rather than implementing MS-NRBF in general. The
 * layout, in record order:
 *
 *   SerializedStreamHeader        root id 1, header -1, version 1.0
 *   SystemClassWithMembersAndTypes  id 1, System.CurrentSystemTimeZone
 *       m_CachedDaylightChanges   SystemClass  -> reference to the hashtable
 *       m_ticksOffset             Int64        inline
 *       m_standardName            String       inline, or a null record
 *       m_daylightName            String       inline, or a null record
 *   SystemClassWithMembersAndTypes  id 2, System.Collections.Hashtable
 *       LoadFactor Single, Version Int32, Comparer and HashCodeProvider null,
 *       HashSize Int32, Keys and Values object arrays by reference
 *   ArraySingleObject             the empty Keys array
 *   ArraySingleObject             the empty Values array
 *   MessageEnd
 *
 * Object ids are assigned in the order records appear, so a graph carrying
 * both names uses 3 and 4 for the strings and 5 and 6 for the arrays, while
 * one with neither name uses 3 and 4 for the arrays. That is what .NET's own
 * BinaryFormatter produces, and the writer here matches it byte for byte.
 */

/* tzset and tzname are POSIX but the `timezone` global is XSI, and the library
 * builds as strict C11 with extensions off, so glibc needs telling before
 * <time.h> is reached. _POSIX_C_SOURCE alone is not enough for `timezone`. */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 700
#endif

#include <string.h>

#include "psrp/psrp_timezone.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

/* MS-NRBF record types, only the ones this graph uses. */
#define NRBF_SERIALIZED_STREAM_HEADER   0
#define NRBF_SYSTEM_CLASS_WITH_MEMBERS  4
#define NRBF_BINARY_OBJECT_STRING       6
#define NRBF_MEMBER_REFERENCE           9
#define NRBF_OBJECT_NULL               10
#define NRBF_MESSAGE_END               11
#define NRBF_ARRAY_SINGLE_OBJECT       16

/* BinaryTypeEnum */
#define NRBF_TYPE_PRIMITIVE     0
#define NRBF_TYPE_STRING        1
#define NRBF_TYPE_SYSTEM_CLASS  3
#define NRBF_TYPE_OBJECT_ARRAY  5

/* PrimitiveTypeEnum */
#define NRBF_PRIM_INT32   8
#define NRBF_PRIM_INT64   9
#define NRBF_PRIM_SINGLE 11

#define TZ_CLASS  "System.CurrentSystemTimeZone"
#define HT_CLASS  "System.Collections.Hashtable"
#define ICOMPARER "System.Collections.IComparer"
#define IHASHER   "System.Collections.IHashCodeProvider"

/* .NET's default Hashtable load factor, 0.72f, as it lands on the wire. */
static const uint8_t kLoadFactor[4] = { 0xEC, 0x51, 0x38, 0x3F };

/* ------------------------------------------------------------- writing -- */

static psrp_result_t put_u8(psrp_buffer_t *b, uint8_t v)
{
    return psrp_buffer_append(b, &v, 1);
}

static psrp_result_t put_i32(psrp_buffer_t *b, int32_t v)
{
    uint8_t raw[4];
    uint32_t u = (uint32_t)v;
    raw[0] = (uint8_t)(u      );
    raw[1] = (uint8_t)(u >>  8);
    raw[2] = (uint8_t)(u >> 16);
    raw[3] = (uint8_t)(u >> 24);
    return psrp_buffer_append(b, raw, sizeof raw);
}

static psrp_result_t put_i64(psrp_buffer_t *b, int64_t v)
{
    uint8_t raw[8];
    uint64_t u = (uint64_t)v;
    int i;
    for (i = 0; i < 8; i++) raw[i] = (uint8_t)(u >> (8 * i));
    return psrp_buffer_append(b, raw, sizeof raw);
}

/* NRBF strings carry a 7-bit encoded length, the same encoding .NET's
 * BinaryWriter uses, followed by UTF-8 bytes. */
static psrp_result_t put_string(psrp_buffer_t *b, const char *s, size_t n)
{
    uint8_t len[5];
    size_t i = 0;
    size_t v = n;
    psrp_result_t rc;

    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        len[i++] = byte;
    } while (v && i < sizeof len);
    if (v) return PSRP_ERR_INVALID_ARG;   /* absurdly long, not a real name */

    rc = psrp_buffer_append(b, len, i);
    if (rc == PSRP_OK && n) rc = psrp_buffer_append(b, s, n);
    return rc;
}

static psrp_result_t put_cstring(psrp_buffer_t *b, const char *s)
{
    return put_string(b, s, strlen(s));
}

#define TRY(expr) do { rc = (expr); if (rc != PSRP_OK) return rc; } while (0)

psrp_result_t psrp_timezone_serialize(const psrp_timezone_t *tz,
                                      psrp_buffer_t *out)
{
    psrp_result_t rc;
    int32_t next_id = 3;      /* 1 is the time zone, 2 the hashtable */
    int32_t keys_id, values_id;

    if (!tz || !out) return PSRP_ERR_INVALID_ARG;

    /* SerializedStreamHeader. */
    TRY(put_u8(out, NRBF_SERIALIZED_STREAM_HEADER));
    TRY(put_i32(out, 1));      /* RootId */
    TRY(put_i32(out, -1));     /* HeaderId */
    TRY(put_i32(out, 1));      /* MajorVersion */
    TRY(put_i32(out, 0));      /* MinorVersion */

    /* The time zone class itself. */
    TRY(put_u8(out, NRBF_SYSTEM_CLASS_WITH_MEMBERS));
    TRY(put_i32(out, 1));                  /* ObjectId */
    TRY(put_cstring(out, TZ_CLASS));
    TRY(put_i32(out, 4));                  /* MemberCount */
    TRY(put_cstring(out, "m_CachedDaylightChanges"));
    TRY(put_cstring(out, "m_ticksOffset"));
    TRY(put_cstring(out, "m_standardName"));
    TRY(put_cstring(out, "m_daylightName"));
    /* BinaryTypeEnums, then the extra information each one needs. */
    TRY(put_u8(out, NRBF_TYPE_SYSTEM_CLASS));
    TRY(put_u8(out, NRBF_TYPE_PRIMITIVE));
    TRY(put_u8(out, NRBF_TYPE_STRING));
    TRY(put_u8(out, NRBF_TYPE_STRING));
    TRY(put_cstring(out, HT_CLASS));       /* for the SystemClass member */
    TRY(put_u8(out, NRBF_PRIM_INT64));     /* for the primitive member */
    /* String members carry no additional information. */

    /* Values, in member order. */
    TRY(put_u8(out, NRBF_MEMBER_REFERENCE));
    TRY(put_i32(out, 2));                  /* the hashtable, defined below */
    TRY(put_i64(out, tz->ticks_offset));

    if (tz->has_standard_name) {
        TRY(put_u8(out, NRBF_BINARY_OBJECT_STRING));
        TRY(put_i32(out, next_id++));
        TRY(put_cstring(out, tz->standard_name));
    } else {
        TRY(put_u8(out, NRBF_OBJECT_NULL));
    }
    if (tz->has_daylight_name) {
        TRY(put_u8(out, NRBF_BINARY_OBJECT_STRING));
        TRY(put_i32(out, next_id++));
        TRY(put_cstring(out, tz->daylight_name));
    } else {
        TRY(put_u8(out, NRBF_OBJECT_NULL));
    }

    keys_id = next_id++;
    values_id = next_id++;

    /* The cache hashtable, written empty. */
    TRY(put_u8(out, NRBF_SYSTEM_CLASS_WITH_MEMBERS));
    TRY(put_i32(out, 2));
    TRY(put_cstring(out, HT_CLASS));
    TRY(put_i32(out, 7));
    TRY(put_cstring(out, "LoadFactor"));
    TRY(put_cstring(out, "Version"));
    TRY(put_cstring(out, "Comparer"));
    TRY(put_cstring(out, "HashCodeProvider"));
    TRY(put_cstring(out, "HashSize"));
    TRY(put_cstring(out, "Keys"));
    TRY(put_cstring(out, "Values"));
    TRY(put_u8(out, NRBF_TYPE_PRIMITIVE));      /* LoadFactor */
    TRY(put_u8(out, NRBF_TYPE_PRIMITIVE));      /* Version */
    TRY(put_u8(out, NRBF_TYPE_SYSTEM_CLASS));   /* Comparer */
    TRY(put_u8(out, NRBF_TYPE_SYSTEM_CLASS));   /* HashCodeProvider */
    TRY(put_u8(out, NRBF_TYPE_PRIMITIVE));      /* HashSize */
    TRY(put_u8(out, NRBF_TYPE_OBJECT_ARRAY));   /* Keys */
    TRY(put_u8(out, NRBF_TYPE_OBJECT_ARRAY));   /* Values */
    TRY(put_u8(out, NRBF_PRIM_SINGLE));
    TRY(put_u8(out, NRBF_PRIM_INT32));
    TRY(put_cstring(out, ICOMPARER));
    TRY(put_cstring(out, IHASHER));
    TRY(put_u8(out, NRBF_PRIM_INT32));
    /* Object arrays carry no additional information. */

    TRY(psrp_buffer_append(out, kLoadFactor, sizeof kLoadFactor));
    TRY(put_i32(out, 0));                  /* Version */
    /* 2.2.3.10.2: both of these MUST be null. */
    TRY(put_u8(out, NRBF_OBJECT_NULL));    /* Comparer */
    TRY(put_u8(out, NRBF_OBJECT_NULL));    /* HashCodeProvider */
    TRY(put_i32(out, 3));                  /* HashSize, .NET's initial bucket count */
    TRY(put_u8(out, NRBF_MEMBER_REFERENCE));
    TRY(put_i32(out, keys_id));
    TRY(put_u8(out, NRBF_MEMBER_REFERENCE));
    TRY(put_i32(out, values_id));

    /* The two empty arrays, then the end of the stream. */
    TRY(put_u8(out, NRBF_ARRAY_SINGLE_OBJECT));
    TRY(put_i32(out, keys_id));
    TRY(put_i32(out, 0));
    TRY(put_u8(out, NRBF_ARRAY_SINGLE_OBJECT));
    TRY(put_i32(out, values_id));
    TRY(put_i32(out, 0));
    TRY(put_u8(out, NRBF_MESSAGE_END));
    return PSRP_OK;
}

/* ------------------------------------------------------------- reading -- */

typedef struct {
    const uint8_t *p;
    size_t left;
    bool bad;
} reader_t;

static uint8_t take_u8(reader_t *r)
{
    if (r->left < 1) { r->bad = true; return 0; }
    r->left--;
    return *r->p++;
}

static int32_t take_i32(reader_t *r)
{
    uint32_t v;
    if (r->left < 4) { r->bad = true; return 0; }
    v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
        ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    r->left -= 4;
    return (int32_t)v;
}

static int64_t take_i64(reader_t *r)
{
    uint64_t v = 0;
    int i;
    if (r->left < 8) { r->bad = true; return 0; }
    for (i = 7; i >= 0; i--) v = (v << 8) | r->p[i];
    r->p += 8;
    r->left -= 8;
    return (int64_t)v;
}

/* Returns a pointer into the input; the caller must copy what it keeps. */
static const char *take_string(reader_t *r, size_t *len)
{
    size_t n = 0;
    unsigned shift = 0;
    const char *s;

    for (;;) {
        uint8_t byte = take_u8(r);
        if (r->bad) return NULL;
        n |= (size_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift > 28) { r->bad = true; return NULL; }
    }
    if (n > r->left) { r->bad = true; return NULL; }
    s = (const char *)r->p;
    r->p += n;
    r->left -= n;
    if (len) *len = n;
    return s;
}

static bool string_is(const char *s, size_t n, const char *want)
{
    return s && strlen(want) == n && memcmp(s, want, n) == 0;
}

/* Copies a name in, refusing one that will not fit rather than truncating it
 * into something that looks like a different time zone. */
static bool copy_name(char *dst, const char *src, size_t n)
{
    if (n >= PSRP_TIMEZONE_NAME_MAX) return false;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

psrp_result_t psrp_timezone_parse(const void *data, size_t len,
                                  psrp_timezone_t *out)
{
    reader_t r;
    const char *s;
    size_t n = 0;
    int32_t members;
    uint8_t tag;
    int i;
    static const char *const kMembers[4] = {
        "m_CachedDaylightChanges", "m_ticksOffset",
        "m_standardName", "m_daylightName"
    };

    if (!data || !out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    r.p = (const uint8_t *)data;
    r.left = len;
    r.bad = false;

    if (take_u8(&r) != NRBF_SERIALIZED_STREAM_HEADER) return PSRP_ERR_MALFORMED;
    (void)take_i32(&r);                       /* RootId */
    (void)take_i32(&r);                       /* HeaderId */
    if (take_i32(&r) != 1) return PSRP_ERR_MALFORMED;   /* MajorVersion */
    (void)take_i32(&r);                       /* MinorVersion */

    if (take_u8(&r) != NRBF_SYSTEM_CLASS_WITH_MEMBERS) return PSRP_ERR_MALFORMED;
    (void)take_i32(&r);                       /* ObjectId */
    s = take_string(&r, &n);
    if (!string_is(s, n, TZ_CLASS)) return PSRP_ERR_MALFORMED;

    members = take_i32(&r);
    if (r.bad || members != 4) return PSRP_ERR_MALFORMED;
    for (i = 0; i < 4; i++) {
        s = take_string(&r, &n);
        if (!string_is(s, n, kMembers[i])) return PSRP_ERR_MALFORMED;
    }

    /* The member types are fixed by 2.2.3.10.1; anything else is a graph we
     * do not know how to walk. */
    if (take_u8(&r) != NRBF_TYPE_SYSTEM_CLASS) return PSRP_ERR_MALFORMED;
    if (take_u8(&r) != NRBF_TYPE_PRIMITIVE) return PSRP_ERR_MALFORMED;
    if (take_u8(&r) != NRBF_TYPE_STRING) return PSRP_ERR_MALFORMED;
    if (take_u8(&r) != NRBF_TYPE_STRING) return PSRP_ERR_MALFORMED;
    s = take_string(&r, &n);                  /* the hashtable's class name */
    if (!string_is(s, n, HT_CLASS)) return PSRP_ERR_MALFORMED;
    if (take_u8(&r) != NRBF_PRIM_INT64) return PSRP_ERR_MALFORMED;

    /* m_CachedDaylightChanges. Only a reference is expected, and the cache is
     * explicitly ignorable, so its contents are never walked. */
    tag = take_u8(&r);
    if (tag == NRBF_MEMBER_REFERENCE) (void)take_i32(&r);
    else if (tag != NRBF_OBJECT_NULL) return PSRP_ERR_MALFORMED;

    out->ticks_offset = take_i64(&r);

    for (i = 0; i < 2; i++) {
        char *dst = i == 0 ? out->standard_name : out->daylight_name;
        bool *have = i == 0 ? &out->has_standard_name : &out->has_daylight_name;

        tag = take_u8(&r);
        if (tag == NRBF_OBJECT_NULL) continue;
        if (tag != NRBF_BINARY_OBJECT_STRING) return PSRP_ERR_MALFORMED;
        (void)take_i32(&r);                   /* ObjectId */
        s = take_string(&r, &n);
        if (r.bad) return PSRP_ERR_MALFORMED;
        if (!copy_name(dst, s, n)) return PSRP_ERR_MALFORMED;
        *have = true;
    }

    if (r.bad) return PSRP_ERR_MALFORMED;
    /* Everything after this is the ignorable cache, so the stream is not
     * walked further. */
    return PSRP_OK;
}

/* ------------------------------------------------------------- current -- */

#ifdef _WIN32
/* Windows reports zone names as UTF-16; PSRP strings are UTF-8. */
static bool wide_to_utf8(const WCHAR *w, char *out, size_t out_size)
{
    int n;
    if (!w || !w[0]) return false;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_size, NULL, NULL);
    return n > 0;
}

psrp_result_t psrp_timezone_current(psrp_timezone_t *out)
{
    TIME_ZONE_INFORMATION tzi;
    DWORD rc;

    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    rc = GetTimeZoneInformation(&tzi);
    if (rc == TIME_ZONE_ID_INVALID) return PSRP_ERR_INTERNAL;

    /* Windows reports Bias as minutes to add to local time to reach UTC, so a
     * zone ahead of UTC has a negative Bias. .NET's m_ticksOffset runs the
     * other way, hence the negation. Getting this backwards would put the
     * remote session a whole day out for zones near the date line. */
    out->ticks_offset = -(int64_t)tzi.Bias * 60 * 10000000;

    out->has_standard_name =
        wide_to_utf8(tzi.StandardName, out->standard_name,
                     sizeof out->standard_name);
    out->has_daylight_name =
        wide_to_utf8(tzi.DaylightName, out->daylight_name,
                     sizeof out->daylight_name);
    return PSRP_OK;
}
#else
/* POSIX. The names come from tzname rather than from a zone database, so they
 * are the abbreviations the C library reports ("GMT", "CEST") where Windows
 * gives full names ("W. Europe Standard Time"). Both are legal here: 2.2.3.10
 * places no constraint on the strings, .NET fills them lazily, and the server
 * does not parse them. The offset is the field that matters and it is exact.
 */
psrp_result_t psrp_timezone_current(psrp_timezone_t *out)
{
    if (!out) return PSRP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    tzset();

    /* POSIX `timezone` is seconds WEST of UTC for standard time, so a zone
     * ahead of UTC is negative; .NET's m_ticksOffset runs the other way.
     * Hence the same negation as the Windows branch, and for the same reason:
     * getting it backwards puts the remote session a day out near the date
     * line. Standard time deliberately -- the field ignores daylight saving,
     * which is why tm_gmtoff would be the wrong source. */
    out->ticks_offset = -(int64_t)timezone * 10000000;

    if (tzname[0] && tzname[0][0]) {
        strncpy(out->standard_name, tzname[0], sizeof out->standard_name - 1);
        out->has_standard_name = true;
    }
    if (tzname[1] && tzname[1][0]) {
        strncpy(out->daylight_name, tzname[1], sizeof out->daylight_name - 1);
        out->has_daylight_name = true;
    }
    return PSRP_OK;
}
#endif
