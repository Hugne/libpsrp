# libpsrp

A C implementation of the PowerShell Remoting Protocol ([MS-PSRP]), client
side, as a static library with a sans-IO core and a pluggable transport.

Every client-side section of the specification is implemented and tested. See
`SPEC-COVERAGE.md` for the per-section status and `TODO.md` for the handful of
places where the spec and real PowerShell disagree.

**It works against real PowerShell.** The interop test opens a RunspacePool on
a live WinRM endpoint, runs a pipeline, disconnects, reconnects, and runs
another pipeline through the same pool:

```
    pool -> Opened
    server protocolversion 2.2
    running $env:COMPUTERNAME
    pipeline -> Completed
    output: "CLAUDE"
    disconnecting
    reconnecting
    running 2 + 2 on the reconnected pool
    pipeline -> Completed
    output: "4"
    shell closed
```

## What it talks to

The client announces protocol version 2.2, PSVersion 2.0 and serialization
version 1.1.0.1, which is what every PowerShell since 3.0 speaks. It accepts a
server announcing protocol major 2, minor 1 or above.

| Server | Protocol | Status |
|---|---|---|
| Windows PowerShell 5.1 | 2.3 | verified live, continuously |
| Windows PowerShell 3.0 / 4.0 | 2.2 | expected; not verified here |
| PowerShell 7.x | 2.3 (2.4 in 7.6) | expected; not verified here |
| Windows PowerShell 2.0 | 2.1 | expected to negotiate; untested, and see the gated features below |

Only the 5.1 row is measured. Everything in `tests/interop` runs against a
Windows PowerShell 5.1 endpoint (5.1.26100.9168) on every change; the other
rows are inferences from the protocol versions those releases speak, not
claims of testing.

A server picks the version, within limits: PowerShell echoes the client's
protocol version back whenever it is 2.0 to 2.3. Because this client offers
2.2, a modern server answers 2.2 even though it could do more. Its real ceiling
is only visible by offering something higher — patching the offer to 2.4 makes
a 5.1 server answer with its true 2.3.

Three messages are gated on the negotiated version, so against an old server
the corresponding calls will fail rather than silently do nothing:

- `INFORMATION_RECORD` (2.2.2.26) needs 2.3. Below that, `Write-Information`
  has no stream to arrive on.
- `CONNECT_RUNSPACEPOOL` and `RUNSPACEPOOL_INIT_DATA` (2.2.2.29, 2.2.2.30) need
  2.2, so adopting another client's disconnected pool does not exist on
  PowerShell 2.0.
- `RESET_RUNSPACE_STATE` (2.2.2.31) needs 2.3; the spec's product note lists
  everything from Windows 7 through Windows Server 2012 R2 as lacking it. The
  5.1 server here honours it even though it negotiated 2.2 with us, so it gates
  on its own 2.3 capability rather than on the negotiated version. Do not read
  that as a guarantee.

Raising the announced version is not just a constant: protocol 2.4 (PowerShell
7.6) deprecates the session key exchange, so the crypto path would have to
become conditional on what was negotiated. See TODO PSRP-28.

## Requirements

Windows only, and deliberately so: the transport is the Win32 WSMan client
(`WsmSvc`), XML is XmlLite, crypto is CNG (`bcrypt`), and shell enumeration
uses the WSMan COM automation interface (`ole32`, `oleaut32`). Porting off
Windows means replacing the transport and the XML backend together — see TODO
PSRP-03 and PSRP-05.

Building needs CMake 3.20+, Ninja, and a C11 compiler; MSVC and clang are both
kept green, with warnings as errors. The interop tests additionally need a
reachable WinRM listener and credentials, and skip themselves without them.

## Example

`examples/run_command.c` is the whole shape of using the library in about a
hundred lines:

```
example_run_command http://localhost:5985/wsman Administrator pw "Get-Date; 6*7"
2026-09-05
42
```

## Against a real server

Unit tests prove an encoding matches the specification. They cannot prove a
server accepts it, or that a feature is reachable through the public API at
all, and that second failure turned out to be the common one: four features
passed every unit test while being impossible to use. So every feature is also
driven against a live PowerShell:

```
PSRP_INTEROP=1 PSRP_USER=... PSRP_PASS=... test_features   # all 16 sections
test_features hostread                                       # just one
```

## Hardening

Everything a server sends is untrusted, so every parser is fuzzed:

```
scripts\build-asan.bat        # AddressSanitizer build, runs the fuzz label
ctest -L fuzz                 # just the fuzzer
PSRP_FUZZ_ITERATIONS=1000000 PSRP_FUZZ_SEED=42 fuzz_parsers   # a soak
```

The fuzzer is deterministic, so a failure reproduces from its seed. It also
reports how often each target actually accepted its input and fails if any
target never did: a fuzzer that only ever exercises rejection paths looks
identical to one that found nothing.

There is also a differential test against `psrpcore`, an independent Python
implementation of the same specification:

```
pip install psrpcore
python tools/differential.py            # regenerate the corpus, check both ways
```

It runs both directions, because they fail differently: psrpcore serializes and
we parse (catching an over-strict reader), and we serialize and psrpcore parses
(catching a writer only our own reader would accept). Direction A is baked into
a committed corpus so `ctest` needs neither Python nor psrpcore.

Some defects only appear across repetition, so there is a lifecycle stress
test alongside the interop one:

```
PSRP_INTEROP=1 PSRP_STRESS_CYCLES=200 test_stress
```

It runs the ordinary open/run/close cycle in a loop and asserts that reusing a
transport does not grow the process handle count. That is how a Receive
cancellation race was found: it failed about one run in a hundred cycles and no
single pass could see it.

Leaks are caught separately. AddressSanitizer on Windows has no leak detector,
so the tests and the fuzzer turn on the MSVC debug CRT's allocation tracking
instead; a debug MSVC run reports `(leak-checked)` when it is active.

## Layout

- `include/psrp/` — the public API.
- `src/core/` — buffers, base64, hex, UTF-8/UTF-16, GUIDs.
- `src/proto/` — fragments, message header, CLIXML, typed message bodies.
- `src/state/` — the client state machine (no I/O).
- `src/transport/` — WSMan transport.
- `src/xml/` — XmlLite backend behind a pull-parser seam.
- `PLAN.md`, `SPEC-COVERAGE.md`, `TODO.md` — design, per-section coverage,
  and everything deliberately deferred.
- `reference/doc/` — the [MS-PSRP] specification (PDF plus extracted text).

## Build

    scripts\build.bat          # MSVC:  vcvars -> cmake -G Ninja -> ninja -> ctest
    scripts\build-clang.bat    # clang: same, with llvm-mingw

Both toolchains are kept green with warnings as errors.

## Testing

CTest drives unit, round-trip, golden-vector and interop tests. The default
run needs no WinRM: the live test skips itself unless enabled.

    ctest --test-dir build\msvc                       # unit tests only
    ctest --test-dir build\msvc -L interop            # live test

To run the live test:

    set PSRP_INTEROP=1
    set PSRP_USER=Administrator
    set PSRP_PASS=...
    ctest --test-dir build\msvc -L interop --output-on-failure

`PSRP_CONNECTION` overrides the endpoint, which defaults to
`http://localhost:5985/wsman`.

## Design

The protocol core performs no I/O. It is a state machine: the caller pumps
received bytes in with `psrp_session_receive`, pulls bytes out with the
payload functions, and reads results from an event queue. That makes the whole
protocol testable without a network, and it is why the suite can drive a full
conversation against a scripted in-memory server.

Serialization behaviour is pinned against real PowerShell output rather than
the spec's prose, which is looser than the implementation in several places.
