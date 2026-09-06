# libpsrp

[![ci](https://github.com/Hugne/libpsrp/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Hugne/libpsrp/actions/workflows/ci.yml)
[![docs](https://github.com/Hugne/libpsrp/actions/workflows/docs.yml/badge.svg?branch=master)](https://github.com/Hugne/libpsrp/actions/workflows/docs.yml)
[![API reference](https://img.shields.io/badge/API-reference-blue)](https://hugne.github.io/libpsrp/)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

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
    output: "WINBOX01"
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

## Two headers for I/O

`psrp_transport.h` is the contract the protocol needs from whatever carries
its bytes: open a session, start a pipeline, push, pull, shut down. It knows
nothing about how they travel, and constructing a transport is deliberately
not part of it -- every carrier needs different things to be built.

`psrp_winrm.h` is WS-Management's half: the configuration and constructor,
the disconnect/reconnect/connect operations that only WSMan provides, and
shell enumeration. A caller includes it to build a transport and then drives
the result through the generic header.

The split is not only tidiness. Shell enumeration exists on Windows and not
in the curl transport, and while both lived in one header the declaration was
visible where the definition was not -- so a Linux caller got an unresolved
symbol at link time with nothing to explain it. Declared where it is
implemented, the same call now fails to compile, at the call site, naming the
function.

## Requirements

To talk to a server, Windows: the transport is the Win32 WSMan client
(`WsmSvc`), crypto is CNG (`bcrypt`), and shell enumeration uses the WSMan COM
automation interface (`ole32`, `oleaut32`).

On Linux it needs libxml2, OpenSSL, libcurl and MIT Kerberos, and NTLM
additionally wants the `gss-ntlmssp` mechanism installed at run time. That
build is a real subset, honestly labelled: the protocol core and crypto are
complete, and the curl transport authenticates, encrypts, opens a RunspacePool,
runs pipelines and reads their output from a live server. Not there yet:
disconnect, reconnect and connect answer `PSRP_ERR_UNSUPPORTED` rather than
pretending, and shell enumeration is Windows-only. See TODO PSRP-35.

On Linux the two dependencies can be linked from their static archives, one
flag per library:

    cmake -S . -B build -DPSRP_STATIC_LIBXML2=ON     # no libxml2.so at runtime
    cmake -S . -B build -DPSRP_STATIC_OPENSSL=ON

They are separate flags because the two are not equally available. libxml2
links statically almost anywhere. A static libcrypto needs the distribution to
have packaged archives for OpenSSL's *own* private dependencies, and Debian
and Ubuntu do not ship `libjitterentropy.a` at all, which makes it unbuildable
there; `PSRP_STATIC_OPENSSL` detects that during configure and says so, rather
than producing a wall of undefined `jent_*` symbols at link time. A single
combined flag would have made the easy half unavailable because of the hard
half.

Neither produces a fully static binary: libc stays dynamic, deliberately,
since statically linking glibc breaks NSS and locale lookups at run time.
What they remove is the dependencies this library chose.

Building needs CMake 3.20+, Ninja, and a C11 compiler; MSVC and clang are both
kept green, with warnings as errors. The interop tests additionally need a
reachable WinRM listener and credentials, and skip themselves without them.

## Two APIs

The library is a protocol implementation first: the session performs no I/O,
so a caller moves bytes between it and a transport and reads an event queue.
That is the right shape for implementing PSRP and the wrong shape for running
a command, so there is a convenience layer on top of it.

**`psrp_client.h`** — connect, run commands, read output:

```c
psrp_client_connect(&cfg, &c);
psrp_client_run(c, "Get-Process | Select-Object -First 3", &r);
/* r.output holds three objects; r.errors, r.warnings and the rest
   stay separate; psrp_run_result_text flattens if that is all you want */
psrp_run_result_free(&r);
psrp_client_free(c);
```

Commands that consume input take it as objects, or as raw bytes for the
common case:

```c
psrp_client_run_bytes(c, script, blob, len, &r);   /* one CLIXML <BA>  */
psrp_client_run_input(c, script, values, n, &r);   /* n objects        */
```

so a sequence of ordinary commands, one that eats a binary blob, and more
ordinary commands all run against the same live runspace.

One client keeps one RunspacePool open, so further commands cost a pipeline
rather than a whole remote shell. That is the substantive reason to prefer it,
ahead of the line count. It deliberately does not cover host callbacks,
disconnect and reconnect, secure strings or command metadata;
`psrp_client_session` and `psrp_client_transport` hand back the objects
underneath so those stay reachable without rewriting anything.

The layer is written entirely against the public API below it and adds no
entry point into the state machine, so it is a convenience rather than a
privileged path.

**Everything else** is that lower level, and `examples/run_command.c` is the
honest picture of it in about a hundred lines: the session, the transport, and
the pump loop that joins them.

```
example_quick_run  http://localhost:5985/wsman Administrator pw "1..3" "$PID"
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
- `src/client/` — the convenience layer, built on the public API.
- `src/transport/` — WinRM transports: a shim over the OS client on
  Windows, and libcurl + GSS-API elsewhere.
- `src/xml/` — XmlLite backend behind a pull-parser seam.
- `PLAN.md`, `SPEC-COVERAGE.md`, `TODO.md` — design, per-section coverage,
  and everything deliberately deferred.
- `examples/` — `run_command.c` (the protocol, explicitly) and
  `quick_run.c` (the same job through `psrp_client.h`).

## Build

    scripts\build.bat          # MSVC:  vcvars -> cmake -G Ninja -> ninja -> ctest
    scripts\build-clang.bat    # clang: same, with llvm-mingw

Both toolchains are kept green with warnings as errors.

## Documentation

The API reference is published at **https://hugne.github.io/libpsrp/**.

The public headers carry that documentation, and Doxygen turns them into a
browsable site:

    scriptsuild-docs.bat      # -> build\doc\html\index.html

It needs doxygen on PATH (`winget install DimitriVanHeesch.Doxygen`); graphviz
is not used. `.github/workflows/docs.yml` runs the same generation on every
push and publishes the result to GitHub Pages.

The script fails on any doxygen warning, deliberately. Comments here are full
of CLIXML element names, and `<b>`, `<c>` and `<s>` are real HTML tags that
doxygen will silently apply to the rendered page rather than print. Element
names in comments therefore go in backticks, and a warning means a page is
being quietly mangled.

Only `include/psrp` is scanned. The implementation under `src/` is commented
for people reading it, not for publication.

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

## Continuous integration

`.github/workflows/ci.yml` builds and tests on every push and pull request:

- **MSVC** and **clang (llvm-mingw)**, the same two toolchains the local
  scripts drive, so a push cannot land warnings that only one compiler sees.
- **interop against a live server.** The runner enables PowerShell remoting on
  itself and the suite connects to `localhost:5985` as the current user over
  Negotiate, so no credential is stored anywhere. This is the job worth having:
  unit tests prove an encoding, and only a real server proves a feature is
  reachable. It gates like any other job: verified passing on a hosted runner
  before being trusted (TODO PSRP-31).

`.github/workflows/docs.yml` publishes the API reference to GitHub Pages.

## Design

The protocol core performs no I/O. It is a state machine: the caller pumps
received bytes in with `psrp_session_receive`, pulls bytes out with the
payload functions, and reads results from an event queue. That makes the whole
protocol testable without a network, and it is why the suite can drive a full
conversation against a scripted in-memory server.

The project began as a lab that drove a local PowerShell over its stdin pipe,
feeding statements one line at a time and pushing raw bytes at a cmdlet that
read them back from the console's own standard-input handle. That mechanism has no
PSRP equivalent -- there is no process whose stdin a client can write to -- but
`tests/interop/test_binary_roundtrip.c` keeps what the lab was actually
testing: the same 8..16384 byte sweep, the same deterministic payload, exact
length and SHA256 verified, and a liveness check afterwards. Only the route the
bytes take changed, from a pipe to a CLIXML `<BA>` carried as pipeline input.

Since the Linux port, every test and a parser soak also run under valgrind in
CI, and any output on stderr fails the job as well. The Windows build had
debug-CRT leak checking already; what memcheck adds is reads of uninitialised
memory and invalid accesses, which matter most in the parsers because that is
the code a hostile server reaches first. It found nothing on the first run
across 24 binaries and 450,000 fuzz iterations -- but it did catch libxml2
printing parse diagnostics to the caller's stderr, which XmlLite never did.

Serialization behaviour is pinned against real PowerShell output rather than
the spec's prose, which is looser than the implementation in several places.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

Apache-2.0 rather than MIT for one reason worth stating: it grants patent
rights explicitly. This is an implementation of a protocol Microsoft holds
patents on, published under the Open Specification Promise, and a licence that
is silent on patents leaves that question resting on an implication. Apache-2.0
answers it for contributions made here.

The protocol itself is Microsoft's. [MS-PSRP] is cited throughout by section
number, which its own IP notice permits; the specification document is not
redistributed here.
