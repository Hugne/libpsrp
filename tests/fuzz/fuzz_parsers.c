/* Mutation fuzzer for every parser that touches untrusted bytes.
 *
 * Everything a server sends reaches one of these entry points, so all of them
 * must survive arbitrary input without crashing, reading out of bounds, or
 * looping forever. They are allowed to reject anything; they are not allowed
 * to misbehave.
 *
 * This is a plain deterministic fuzzer rather than libFuzzer, because it has to
 * run as an ordinary ctest under both MSVC and clang with no instrumentation.
 * The seed is fixed so a failure reproduces exactly; PSRP_FUZZ_SEED and
 * PSRP_FUZZ_ITERATIONS override it for longer soak runs. Build with a sanitizer
 * (scripts\build-asan.bat) to make the memory errors it provokes fatal instead
 * of silent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psrp/psrp.h"
#include "psrp/psrp_fragment.h"
#include "psrp/psrp_message.h"
#include "psrp/psrp_clixml.h"
#include "psrp/psrp_messages.h"
#include "psrp/psrp_records.h"
#include "psrp/psrp_metadata.h"
#include "psrp/psrp_host.h"
#include "psrp/psrp_timezone.h"
#include "psrp/psrp_session.h"

/* xorshift64*, so a run reproduces from its seed on any platform. */
static uint64_t g_state;

static uint64_t next_random(void)
{
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return g_state * 2685821657736338717ULL;
}

static size_t random_below(size_t n)
{
    return n ? (size_t)(next_random() % n) : 0;
}

/* ------------------------------------------------------------- corpus -- */

/* Seeds are real messages, so mutations land near valid input rather than
 * bouncing off the first length check. */
static const char *const kXmlSeeds[] = {
    "<S>hello</S>",
    "<Obj RefId=\"0\"><MS><I32 N=\"RunspaceState\">2</I32></MS></Obj>",
    "<Obj RefId=\"0\"><TN RefId=\"0\"><T>System.Object</T></TN>"
    "<ToString>x</ToString><Props><B N=\"b\">true</B></Props>"
    "<MS><Obj N=\"list\" RefId=\"1\"><LST><I32>1</I32><S>two</S></LST></Obj>"
    "</MS></Obj>",
    "<Obj RefId=\"0\"><DCT><En><S N=\"Key\">k</S><I64 N=\"Value\">9</I64></En>"
    "</DCT></Obj>",
    "<Obj RefId=\"0\"><MS><I64 N=\"ci\">1</I64>"
    "<B N=\"SetMinMaxRunspacesResponse\">true</B></MS></Obj>",
    "<Obj RefId=\"0\"><MS><I64 N=\"ci\">3</I64>"
    "<Obj N=\"mi\" RefId=\"1\"><I32>16</I32></Obj>"
    "<Obj N=\"mp\" RefId=\"2\"><LST><S>a</S></LST></Obj></MS></Obj>",
    "<Obj RefId=\"0\"><ToString>bad</ToString><MS>"
    "<S N=\"FullyQualifiedErrorId\">Oops</S>"
    "<I32 N=\"ErrorCategory_Category\">7</I32>"
    "<S N=\"InvocationInfo_InvocationName\">Get-Thing</S></MS></Obj>",
    "<Obj RefId=\"0\"><MS><S N=\"Name\">Get-Thing</S>"
    "<Obj N=\"Parameters\" RefId=\"1\"><DCT><En><S N=\"Key\">P</S>"
    "<Obj N=\"Value\" RefId=\"2\"><MS><S N=\"ParameterType\">System.String</S>"
    "</MS></Obj></En></DCT></Obj></MS></Obj>",
    "<Obj RefId=\"0\"><MS><Version N=\"protocolversion\">2.2</Version>"
    "<Version N=\"PSVersion\">2.0</Version>"
    "<Version N=\"SerializationVersion\">1.1.0.1</Version></MS></Obj>",
    "<Obj RefId=\"0\"><Props><C N=\"character\">65</C>"
    "<Obj N=\"foregroundColor\" RefId=\"1\"><I32>0</I32></Obj></Props></Obj>",
    /* Deliberately awkward shapes: deep nesting and a self-reference. */
    "<Obj RefId=\"0\"><MS><Obj N=\"a\" RefId=\"1\"><MS><Obj N=\"b\" RefId=\"2\">"
    "<MS><Obj N=\"c\" RefId=\"3\"><LST><Ref RefId=\"0\" /></LST></Obj></MS>"
    "</Obj></MS></Obj></MS></Obj>",
    "<Obj RefId=\"0\"><LST><Ref RefId=\"0\" /></LST></Obj>",
    "<S>_x0041_ _xZZZZ_ _x00</S>",
};
#define XML_SEED_COUNT (sizeof kXmlSeeds / sizeof kXmlSeeds[0])

#define MAX_CASE 4096

typedef struct {
    uint8_t data[MAX_CASE];
    size_t len;
} testcase_t;

/* Mutation has to be gentle here. XML is brittle: several edits at once, or a
 * truncation, almost always breaks well-formedness, and then every parser
 * rejects the input at its first check and the run proves nothing. An early
 * version of this fuzzer did exactly that, passing millions of iterations
 * while never once getting a document through. So: one edit, biased toward
 * changing bytes rather than removing them, and structural damage kept rare.
 */
static void mutate(testcase_t *c)
{
    switch (random_below(10)) {
    case 0: case 1: case 2: case 3:               /* flip a bit */
        if (c->len) {
            size_t at = random_below(c->len);
            c->data[at] ^= (uint8_t)(1u << random_below(8));
        }
        break;
    case 4: case 5: case 6:                       /* overwrite a byte */
        if (c->len) c->data[random_below(c->len)] = (uint8_t)next_random();
        break;
    case 7:                                       /* insert a byte */
        if (c->len < MAX_CASE) {
            size_t at = random_below(c->len + 1);
            memmove(c->data + at + 1, c->data + at, c->len - at);
            c->data[at] = (uint8_t)next_random();
            c->len++;
        }
        break;
    case 8:                                       /* truncate */
        if (c->len) c->len = random_below(c->len);
        break;
    default:                                      /* splice a tricky length */
        if (c->len >= 4) {
            size_t at = random_below(c->len - 3);
            static const uint32_t interesting[] = {
                0u, 1u, 0x7Fu, 0x80u, 0xFFu, 0xFFFFu, 0x7FFFFFFFu,
                0x80000000u, 0xFFFFFFFFu
            };
            uint32_t v = interesting[random_below(
                sizeof interesting / sizeof interesting[0])];
            memcpy(c->data + at, &v, 4);
        }
        break;
    }
}

/* ------------------------------------------------------------ targets -- */

/* A fuzzer that never gets past the first length check proves nothing, so
 * every target counts how often its input was actually accepted. The run
 * reports the rates and fails if any target never once succeeded. */
static unsigned long g_attempts[8];
static unsigned long g_accepts[8];


/* Each target takes arbitrary bytes and must return rather than misbehave. */

static void target_clixml(const uint8_t *d, size_t n)
{
    psrp_value_t v;
    psrp_buffer_t out;

    psrp_value_init(&v);
    g_attempts[0]++;
    if (psrp_clixml_deserialize(d, n, &v) == PSRP_OK) {
        g_accepts[0]++;
        /* Anything that parses must also re-serialize without blowing up. */
        psrp_buffer_init(&out);
        (void)psrp_clixml_serialize(&v, &out);
        psrp_buffer_free(&out);
    }
    psrp_value_free(&v);
}

static void target_fragment(const uint8_t *d, size_t n)
{
    psrp_defrag_t *df = psrp_defrag_new();
    psrp_fragment_t f;
    psrp_reader_t r;

    psrp_reader_init(&r, d, n);
    g_attempts[1]++;
    if (psrp_fragment_decode(&r, &f) == PSRP_OK) g_accepts[1]++;

    if (df) {
        /* Feed it in two pieces as well, since a defragmenter's real hazard is
         * state carried across calls, not any single buffer. */
        size_t split = n ? random_below(n) : 0;
        if (psrp_defrag_push(df, d, split) == PSRP_OK &&
            psrp_defrag_push(df, d + split, n - split) == PSRP_OK) {
            for (;;) {
                psrp_buffer_t msg;
                uint64_t oid = 0;
                psrp_result_t rc;
                psrp_buffer_init(&msg);
                rc = psrp_defrag_next(df, &oid, &msg);
                psrp_buffer_free(&msg);
                if (rc != PSRP_OK) break;
            }
        }
        psrp_defrag_free(df);
    }
}

static void target_message(const uint8_t *d, size_t n)
{
    psrp_message_t m;
    g_attempts[2]++;
    if (psrp_message_decode(d, n, &m) == PSRP_OK) {
        const uint8_t *xml = NULL;
        size_t xml_len = 0;
        g_accepts[2]++;
        psrp_message_xml(&m, &xml, &xml_len);
    }
}

static void target_session(const uint8_t *d, size_t n)
{
    /* The whole receive path: defragment, decode, dispatch, drain events. */
    psrp_session_t *s = psrp_session_new();
    psrp_event_t e;

    if (!s) return;
    g_attempts[3]++;
    (void)psrp_session_receive(s, d, n);
    while (psrp_session_next_event(s, &e) == PSRP_OK) {
        g_accepts[3]++;      /* an event means a whole message got through */
        psrp_event_free(&e);
    }
    psrp_session_free(s);
}

static void target_bodies(const uint8_t *d, size_t n)
{
    psrp_session_capability_t cap;
    psrp_runspacepool_state_msg_t rps;
    psrp_pipeline_state_msg_t ps;
    psrp_runspace_availability_t ra;
    psrp_runspacepool_init_data_t init;
    psrp_error_record_t er;
    psrp_informational_record_t ir;
    psrp_progress_record_t pr;
    psrp_information_record_t inf;
    psrp_command_metadata_t cm;
    psrp_user_event_t ue;
    psrp_invocation_info_t ii;
    psrp_host_call_t hc;
    psrp_timezone_t tz;
    int32_t count = 0;
    psrp_value_t v;

    g_attempts[4]++;
    if (psrp_parse_session_capability(d, n, &cap) == PSRP_OK) g_accepts[4]++;
    if (psrp_parse_runspacepool_state(d, n, &rps) == PSRP_OK)
        psrp_runspacepool_state_msg_free(&rps);
    if (psrp_parse_pipeline_state(d, n, &ps) == PSRP_OK)
        psrp_pipeline_state_msg_free(&ps);
    (void)psrp_parse_runspace_availability(d, n, &ra);
    (void)psrp_parse_runspacepool_init_data(d, n, &init);
    if (psrp_parse_error_record(d, n, &er) == PSRP_OK)
        psrp_error_record_free(&er);
    if (psrp_parse_informational_record(d, n, &ir) == PSRP_OK)
        psrp_informational_record_free(&ir);
    if (psrp_parse_progress_record(d, n, &pr) == PSRP_OK)
        psrp_progress_record_free(&pr);
    if (psrp_parse_information_record(d, n, &inf) == PSRP_OK)
        psrp_information_record_free(&inf);
    if (psrp_parse_command_metadata(d, n, &cm) == PSRP_OK)
        psrp_command_metadata_free(&cm);
    (void)psrp_parse_command_metadata_count(d, n, &count);
    if (psrp_parse_user_event(d, n, &ue) == PSRP_OK)
        psrp_user_event_free(&ue);
    if (psrp_parse_invocation_info(d, n, &ii) == PSRP_OK)
        psrp_invocation_info_free(&ii);
    if (psrp_parse_host_call(d, n, &hc) == PSRP_OK)
        psrp_host_call_free(&hc);
    (void)psrp_timezone_parse(d, n, &tz);

    psrp_value_init(&v);
    if (psrp_parse_pipeline_output(d, n, &v) == PSRP_OK) g_accepts[5]++;
    g_attempts[5]++;
    psrp_value_free(&v);
}

typedef void (*target_fn)(const uint8_t *, size_t);

static const struct { const char *name; target_fn fn; } kTargets[] = {
    { "clixml",   target_clixml },
    { "fragment", target_fragment },
    { "message",  target_message },
    { "session",  target_session },
    { "bodies",   target_bodies },
};
#define TARGET_COUNT (sizeof kTargets / sizeof kTargets[0])

/* --------------------------------------------------------------- main -- */

/* Builds a starting case: either an XML seed, or a real fragment carrying one,
 * so the binary framing gets exercised as well as the XML. */
static void seed_case(testcase_t *c)
{
    const char *xml = kXmlSeeds[random_below(XML_SEED_COUNT)];
    size_t xml_len = strlen(xml);

    /* Three shapes, so each layer gets input it can actually accept: bare
     * XML for the serializers, a 40-byte-header message for the message
     * decoder, and a fragmented message for the defragmenter and the session. */
    if (random_below(3) == 0) {
        c->len = xml_len < MAX_CASE ? xml_len : MAX_CASE;
        memcpy(c->data, xml, c->len);
        return;
    }

    {
        bool framed = random_below(2) == 0;
        psrp_message_t m;
        psrp_buffer_t body, wire;

        memset(&m, 0, sizeof m);
        m.destination = PSRP_DEST_CLIENT;
        m.type = PSRP_MSG_PIPELINE_OUTPUT;
        m.rpid = psrp_guid_empty;
        m.pid = psrp_guid_empty;
        m.data = (const uint8_t *)xml;
        m.data_len = xml_len;

        psrp_buffer_init(&body);
        psrp_buffer_init(&wire);
        c->len = 0;
        if (psrp_message_encode(&body, &m) == PSRP_OK) {
            if (!framed) {
                if (body.len <= MAX_CASE) {
                    memcpy(c->data, body.data, body.len);
                    c->len = body.len;
                }
            } else if (psrp_fragment_split(&wire, 1, body.data, body.len, 0)
                           == PSRP_OK && wire.len <= MAX_CASE) {
                memcpy(c->data, wire.data, wire.len);
                c->len = wire.len;
            }
        }
        psrp_buffer_free(&body);
        psrp_buffer_free(&wire);
    }
}

int main(void)
{
    const char *seed_env = getenv("PSRP_FUZZ_SEED");
    const char *iter_env = getenv("PSRP_FUZZ_ITERATIONS");
    unsigned long iterations = iter_env ? strtoul(iter_env, NULL, 10) : 20000;
    uint64_t seed = seed_env ? strtoull(seed_env, NULL, 10) : 0x5DEECE66DULL;
    testcase_t c;
    unsigned long i;

    if (iterations == 0) iterations = 1;
    g_state = seed ? seed : 1;

    printf("fuzzing %u targets, %lu iterations, seed %llu\n",
           (unsigned)TARGET_COUNT, iterations, (unsigned long long)seed);

    memset(&c, 0, sizeof c);
    for (i = 0; i < iterations; i++) {
        size_t t;

        /* Restart from a seed often, so cases stay near valid input instead of
         * drifting into noise that every parser rejects at the first byte. */
        if (i % 8 == 0) seed_case(&c);

        /* Roughly a third of runs use the case untouched. Those are what
         * actually exercise the deep paths; the mutated ones probe the edges
         * around them. Without this the fuzzer only ever tests rejection. */
        if (random_below(3) != 0) mutate(&c);

        t = random_below(TARGET_COUNT);
        kTargets[t].fn(c.data, c.len);
    }

    /* Reaching here without a crash, hang, or sanitizer report is most of the
     * pass. The rest is proving the fuzzer got somewhere: a target that never
     * once accepted its input tested nothing but its own rejection path, and
     * a run like that would look identical to a run that found nothing. */
    {
        static const char *const kNames[6] = {
            "clixml", "fragment", "message", "session-event",
            "capability", "pipeline-output"
        };
        int lean = 0;
        size_t k;

        for (k = 0; k < 6; k++) {
            double pct = g_attempts[k]
                ? (100.0 * (double)g_accepts[k] / (double)g_attempts[k]) : 0.0;
            printf("  %-16s %8lu/%-8lu accepted (%.2f%%)\n",
                   kNames[k], g_accepts[k], g_attempts[k], pct);
            if (g_accepts[k] == 0) lean = 1;
        }
        if (lean) {
            printf("FAIL: a target never accepted any input; the fuzzer is "
                   "not reaching past its first check\n");
            return 1;
        }
    }

    printf("PASS: %lu iterations, no crash\n", iterations);
    return 0;
}
