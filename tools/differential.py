"""Differential testing against psrpcore.

Every golden vector in the test suite came from PowerShell itself, which is the
authoritative oracle but also a single one: a misreading shared between our
code and our reading of PowerShell's output would go unnoticed. psrpcore is an
independent implementation of the same spec, so disagreeing with it is a signal
worth having.

Two directions, because they fail differently:

  A. psrpcore serializes, we parse.  Catches a reader that is too strict, or
     that mis-handles a shape PowerShell happened not to produce in our
     captures.
  B. we serialize, psrpcore parses.  Catches a writer that emits something only
     our own reader would accept.

Direction A is baked into a committed header so ctest stays hermetic and needs
neither Python nor psrpcore. Direction B needs both, so it runs from here.

Usage:
    pip install psrpcore
    python tools/differential.py generate      # rewrite the corpus header
    python tools/differential.py verify [exe]  # check direction B
    python tools/differential.py               # both
"""

import subprocess
import sys
import os
import xml.etree.ElementTree as ET

from psrpcore.types import serialize, deserialize
from psrpcore._crypto import PSRemotingCrypto
from psrpcore.types import (
    PSInt, PSInt64, PSUInt, PSUInt64, PSByte, PSSByte, PSInt16, PSUInt16,
    PSSingle, PSDouble, PSDecimal, PSChar, PSBool, PSString, PSVersion,
    PSGuid, PSUri, PSByteArray, PSDateTime, PSDuration, PSXml,
    PSScriptBlock, PSStack, PSQueue, PSCustomObject,
)

CRYPTO = PSRemotingCrypto()
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, "tests", "unit", "differential_corpus.h")

# (name, value, expected psrp_value kind, expected rendered text or None,
#  buildable)
#
# The text is what psrp_value_to_text produces, so it is only given where the
# rendering is unambiguous. Containers give a count instead.
#
# `buildable` says whether test_differential can reconstruct the value with our
# own API. Only those take part in direction B; the rest are parse-only, which
# is still worth having because direction A is where an over-strict reader
# shows up and shapes like TNRef are cheap to cover that way.
CASES = [
    ("string",          PSString("hello"),            "PSRP_VAL_STRING",  "hello", True),
    ("string_empty",    PSString(""),                 "PSRP_VAL_STRING",  "", True),
    # Escaping is where a serializer most often diverges, so several shapes.
    ("string_control",  PSString("a\x00b"),           "PSRP_VAL_STRING",  None, True),
    ("string_newline",  PSString("one\ntwo"),         "PSRP_VAL_STRING",  "one\ntwo", True),
    ("string_underscore", PSString("_x0041_"),        "PSRP_VAL_STRING",  "_x0041_", True),
    ("string_markup",   PSString("<&>\"'"),           "PSRP_VAL_STRING",  "<&>\"'", True),
    ("string_unicode",  PSString("café 中"), "PSRP_VAL_STRING",  "café 中", True),

    # The wire form is "true"/"false" but psrp_value_to_text renders the
    # PowerShell display form, which is capitalised. Two different questions
    # about the same value; the expectation here is the rendering.
    ("bool_true",       PSBool(True),                 "PSRP_VAL_BOOL",    "True", True),
    ("bool_false",      PSBool(False),                "PSRP_VAL_BOOL",    "False", True),
    ("null",            None,                         "PSRP_VAL_NULL",    None, True),

    ("byte",            PSByte(255),                  "PSRP_VAL_UINT8",   "255", True),
    ("sbyte",           PSSByte(-128),                "PSRP_VAL_INT8",    "-128", True),
    ("int16",           PSInt16(-32768),              "PSRP_VAL_INT16",   "-32768", True),
    ("uint16",          PSUInt16(65535),              "PSRP_VAL_UINT16",  "65535", True),
    ("int32",           PSInt(-2147483648),           "PSRP_VAL_INT32",   "-2147483648", True),
    ("uint32",          PSUInt(4294967295),           "PSRP_VAL_UINT32",  "4294967295", True),
    ("int64",           PSInt64(-9223372036854775808), "PSRP_VAL_INT64",  "-9223372036854775808", True),
    ("uint64",          PSUInt64(18446744073709551615), "PSRP_VAL_UINT64", "18446744073709551615", True),

    ("single",          PSSingle(1.5),                "PSRP_VAL_SINGLE",  None, True),
    ("double",          PSDouble(1.25),               "PSRP_VAL_DOUBLE",  None, True),
    ("decimal",         PSDecimal("1.100"),           "PSRP_VAL_DECIMAL", None, True),
    ("char",            PSChar("A"),                  "PSRP_VAL_CHAR",    None, True),

    ("guid",            PSGuid("4358d2a2-8a0b-4b5d-9c43-1d2e3f405162"),
                                                      "PSRP_VAL_GUID",    None, True),
    ("version",         PSVersion("6.2.1.3"),         "PSRP_VAL_VERSION", "6.2.1.3", True),
    ("uri",             PSUri("http://example.com/a b"), "PSRP_VAL_URI",  None, True),
    ("bytes",           PSByteArray(b"\x00\x01\xfe\xff"), "PSRP_VAL_BYTES", None, True),
    ("duration",        PSDuration(days=1, seconds=3), "PSRP_VAL_DURATION", None, True),
    ("xmldoc",          PSXml("<a b='c'/>"),          "PSRP_VAL_XMLDOC",  None, True),

    ("list",            [PSInt(1), PSString("a")],    "PSRP_VAL_OBJECT",  None, True),
    ("list_nested",     [[PSInt(1)], PSString("x")],  "PSRP_VAL_OBJECT",  None, True),
    ("list_empty",      [],                           "PSRP_VAL_OBJECT",  None, True),
    ("dict",            {PSString("k"): PSString("v")}, "PSRP_VAL_OBJECT", None, True),

    # Parse-only from here: shapes worth reading but not worth hand-building.
    ("datetime",        PSDateTime(2026, 9, 5, 1, 2, 3), "PSRP_VAL_DATETIME", None, False),
    ("scriptblock",     PSScriptBlock("Get-Date"),    "PSRP_VAL_SCRIPTBLOCK", "Get-Date", False),
    ("stack",           PSStack([PSInt(1), PSInt(2)]), "PSRP_VAL_OBJECT",  None, False),
    ("queue",           PSQueue(),                    "PSRP_VAL_OBJECT",  None, False),
    ("custom_object",   PSCustomObject(Name="x", Count=PSInt(3)),
                                                      "PSRP_VAL_OBJECT",  None, False),
    ("dict_nested",     {PSString("a"): [PSInt(1)]},  "PSRP_VAL_OBJECT",  None, False),
    # Nested lists reuse the outer type via <TNRef>, so this is the only case
    # that exercises resolving a type reference back to its definition.
    ("list_type_ref",   [[[PSInt(7)]]],               "PSRP_VAL_OBJECT",  None, False),
]

# Container cases carry an expected item/entry count instead of text.
COUNTS = {
    "list": ("items", 2),
    "stack": ("items", 2),
    "queue": ("items", 0),
    "dict_nested": ("entries", 1),
    "list_type_ref": ("items", 1),
    "list_nested": ("items", 2),
    "list_empty": ("items", 0),
    "dict": ("entries", 1),
}


def clixml_of(value):
    return ET.tostring(serialize(value, CRYPTO), encoding="unicode")


def c_string(s):
    """Encodes a UTF-8 string as a C literal, escaping anything awkward."""
    out = []
    for ch in s.encode("utf-8"):
        if ch == 0x22:
            out.append('\\"')
        elif ch == 0x5C:
            out.append("\\\\")
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            # Hex escapes are terminated by starting a new string literal, so a
            # following hex digit cannot be swallowed into the escape.
            out.append('\\x%02x""' % ch)
    return '"' + "".join(out) + '"'


def generate():
    lines = [
        "/* Generated by tools/differential.py. Do not edit.",
        " *",
        " * CLIXML produced by psrpcore, an independent implementation of the same",
        " * specification. Our reader must accept all of it and agree about what it",
        " * means. See tools/differential.py for why this exists.",
        " */",
        "#ifndef PSRP_DIFFERENTIAL_CORPUS_H",
        "#define PSRP_DIFFERENTIAL_CORPUS_H",
        "",
        "typedef struct {",
        "    const char *name;",
        "    const char *xml;",
        "    size_t xml_len;",
        "    psrp_value_kind_t kind;",
        "    const char *text;      /* expected rendering, or NULL if unchecked */",
        "    size_t text_len;",
        "    long item_count;       /* container items, or -1 */",
        "    long entry_count;      /* dictionary entries, or -1 */",
        "    int buildable;         /* can test_differential reconstruct it? */",
        "} differential_case_t;",
        "",
        "static const differential_case_t kDifferentialCases[] = {",
    ]

    for name, value, kind, text, buildable in CASES:
        xml = clixml_of(value)
        items = entries = -1
        if name in COUNTS:
            which, n = COUNTS[name]
            if which == "items":
                items = n
            else:
                entries = n
        xml_bytes = len(xml.encode("utf-8"))
        if text is None:
            text_c, text_len = "NULL", 0
        else:
            text_c, text_len = c_string(text), len(text.encode("utf-8"))
        lines.append("    { \"%s\"," % name)
        lines.append("      %s," % c_string(xml))
        lines.append("      %du, %s, %s, %du, %d, %d, %d }," %
                     (xml_bytes, kind, text_c, text_len, items, entries,
                      1 if buildable else 0))

    lines += [
        "};",
        "",
        "#define DIFFERENTIAL_CASE_COUNT "
        "(sizeof kDifferentialCases / sizeof kDifferentialCases[0])",
        "",
        "#endif /* PSRP_DIFFERENTIAL_CORPUS_H */",
        "",
    ]

    with open(CORPUS, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d cases)" % (CORPUS, len(CASES)))


def find_exe(argv):
    if len(argv) > 2:
        return argv[2]
    for build in ("msvc", "clang"):
        p = os.path.join(ROOT, "build", build, "tests", "test_differential.exe")
        if os.path.exists(p):
            return p
    return None


def verify(argv):
    """Direction B: our serializer's output, read back by psrpcore."""
    exe = find_exe(argv)
    if not exe:
        print("no test_differential binary; build first")
        return 1

    out = subprocess.run([exe, "--emit"], capture_output=True, text=True,
                         encoding="utf-8")
    if out.returncode != 0:
        print("emit failed:\n" + out.stdout + out.stderr)
        return 1

    by_name = {name: value for name, value, _, _, buildable in CASES
               if buildable}
    failures = 0
    checked = 0
    for line in out.stdout.splitlines():
        if "\t" not in line:
            continue
        name, xml = line.split("\t", 1)
        if name not in by_name:
            continue
        checked += 1
        try:
            got = deserialize(ET.fromstring(xml), CRYPTO)
        except Exception as exc:                      # noqa: BLE001
            print("FAIL %-18s psrpcore could not read our output: %s\n  %s"
                  % (name, exc, xml))
            failures += 1
            continue

        want = by_name[name]
        if not equivalent(got, want):
            print("FAIL %-18s psrpcore read back %r, expected %r\n  %s"
                  % (name, got, want, xml))
            failures += 1

    print("direction B: %d checked, %d disagreed" % (checked, failures))
    return 1 if failures else 0


def equivalent(got, want):
    """Compares across the two type systems without demanding identical types."""
    if want is None:
        return got is None
    if isinstance(want, (list, tuple)):
        if got is None or not hasattr(got, "__len__"):
            return False
        return len(got) == len(want) and all(
            equivalent(g, w) for g, w in zip(got, want))
    if isinstance(want, dict):
        if got is None or not hasattr(got, "keys"):
            return False
        return len(got) == len(want)
    if isinstance(want, (bytes, bytearray)):
        return bytes(got) == bytes(want)
    if isinstance(want, float):
        return abs(float(got) - float(want)) < 1e-9
    if isinstance(want, bool):
        return bool(got) == bool(want)
    if isinstance(want, int):
        return int(got) == int(want)
    return str(got) == str(want)


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "both"
    rc = 0
    if action in ("generate", "both"):
        generate()
    if action in ("verify", "both"):
        rc = verify(sys.argv)
    return rc


if __name__ == "__main__":
    sys.exit(main())
