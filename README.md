# libpsrp

[![ci](https://github.com/Hugne/libpsrp/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Hugne/libpsrp/actions/workflows/ci.yml)
[![docs](https://github.com/Hugne/libpsrp/actions/workflows/docs.yml/badge.svg?branch=master)](https://github.com/Hugne/libpsrp/actions/workflows/docs.yml)
[![API reference](https://img.shields.io/badge/API-reference-blue)](https://hugne.github.io/libpsrp/)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

A C implementation of the PowerShell Remoting Protocol ([MS-PSRP]), client
side, as a static library with a sans-IO core and a pluggable transport.
Windows and Linux.

Every client-side section of the specification is implemented and tested.
`SPEC-COVERAGE.md` has the per-section status; `TODO.md` records every
deferral and every place the spec and real PowerShell disagree.

## Capabilities

- RunspacePool open, close, reset, and adoption of a pool another client
  disconnected from.
- Pipelines: run, stop, pipeline input as objects or as a raw byte array,
  and the six output streams kept separate (output, error, warning, verbose,
  debug, information).
- Host calls and host responses, including reads.
- Session key exchange and SecureString in both directions.
- Command metadata (`GET_COMMAND_METADATA`).
- User events, application private data, runspace availability.
- Disconnect, reconnect and connect, on both platforms.
- Shell enumeration — **Windows only**; it uses the WSMan COM automation
  interface and has no WS-Enumerate implementation yet.

Authentication is NTLM, Kerberos or SPNEGO. On Linux, Kerberos works from a
ticket cache with no password in the process; message encryption is on for
every request on both platforms.

## Servers

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

PowerShell echoes the client's protocol version back whenever it is 2.0 to
2.3, so a modern server answers 2.2 here even though it could do more. Its
real ceiling is only visible by offering something higher.

Three messages are gated on the negotiated version, so against an old server
the corresponding calls fail rather than silently do nothing:

- `INFORMATION_RECORD` (2.2.2.26) needs 2.3. Below that, `Write-Information`
  has no stream to arrive on.
- `CONNECT_RUNSPACEPOOL` and `RUNSPACEPOOL_INIT_DATA` (2.2.2.29, 2.2.2.30)
  need 2.2, so adopting another client's disconnected pool does not exist on
  PowerShell 2.0.
- `RESET_RUNSPACE_STATE` (2.2.2.31) needs 2.3; the spec's product note lists
  everything from Windows 7 through Windows Server 2012 R2 as lacking it. The
  5.1 server here honours it even though it negotiated 2.2, so it gates on its
  own 2.3 capability rather than on the negotiated version. That is not a
  guarantee.

Raising the announced version is not just a constant: protocol 2.4
(PowerShell 7.6) deprecates the session key exchange, so the crypto path would
have to become conditional on what was negotiated. See TODO PSRP-28.

## Requirements

**Windows.** The transport is the Win32 WSMan client (`WsmSvc`), crypto is CNG
(`bcrypt`), XML is XmlLite, and shell enumeration uses the WSMan COM
automation interface (`ole32`, `oleaut32`). No external dependencies.

**Linux.** libxml2, OpenSSL, libcurl and MIT Kerberos. NTLM additionally needs
the `gss-ntlmssp` mechanism installed at run time; Kerberos does not.

Building needs CMake 3.20+, Ninja and a C11 compiler. MSVC and clang are both
kept green with warnings as errors.

Two dependencies can be linked from their static archives, one flag per
library:

    cmake -S . -B build -DPSRP_STATIC_LIBXML2=ON
    cmake -S . -B build -DPSRP_STATIC_OPENSSL=ON

They are separate flags because the two are not equally available. libxml2
links statically almost anywhere. A static libcrypto needs the distribution to
have packaged archives for OpenSSL's own private dependencies, and Debian and
Ubuntu do not ship `libjitterentropy.a` at all; `PSRP_STATIC_OPENSSL` detects
that during configure and reports it, rather than producing undefined `jent_*`
symbols at link time.

Neither produces a fully static binary: libc stays dynamic, deliberately,
because statically linking glibc breaks NSS and locale lookups at run time.

## Two APIs

The library is a protocol implementation first: the session performs no I/O,
so a caller moves bytes between it and a transport and reads an event queue.
There is a convenience layer on top for the common case.

**`psrp_client.h`** — connect, run commands, read output:

```c
psrp_client_connect(&cfg, &c);
psrp_client_run(c, "Get-Process | Select-Object -First 3", &r);
/* r.output holds three objects; r.errors, r.warnings and the rest
   stay separate; psrp_run_result_text flattens if that is all you want */
psrp_run_result_free(&r);
psrp_client_free(c);
```

Commands that consume input take it as objects, or as raw bytes:

```c
psrp_client_run_bytes(c, script, blob, len, &r);   /* one CLIXML <BA>  */
psrp_client_run_input(c, script, values, n, &r);   /* n objects        */
```

One client keeps one RunspacePool open, so further commands cost a pipeline
rather than a whole remote shell. The layer does not cover host callbacks,
disconnect and reconnect, secure strings or command metadata;
`psrp_client_session` and `psrp_client_transport` hand back the objects
underneath so those stay reachable. It is written entirely against the public
API below it and adds no entry point into the state machine.

**Everything else** is that lower level. `examples/run_command.c` shows it in
about a hundred lines: the session, the transport, and the pump loop that
joins them.

```
example_quick_run   http://host:5985/wsman Administrator pw "1..3" "$PID"
example_run_command http://host:5985/wsman Administrator pw "Get-Date; 6*7"
```

## Two headers for I/O

`psrp_transport.h` is the contract the protocol needs from whatever carries
its bytes: open a session, start a pipeline, push, pull, shut down. It knows
nothing about how they travel, and constructing a transport is not part of it.

`winrm.h` is a WS-Management client in its own right: configuration and
constructor, shells, commands, named streams, signals, the
disconnect/reconnect/connect operations, and enumeration. It knows nothing
about PowerShell — identifiers are opaque strings — and everything specific to
PSRP's use of it lives in `src/transport/psrp_over_winrm.c`.

Enumeration is declared only on the platform that implements it, so a Linux
call site fails to compile naming the function, rather than failing to link.

## Testing

CTest drives unit, round-trip, golden-vector, fuzz and interop tests. The
default run needs no WinRM: the live tests skip themselves unless enabled.

    ctest --test-dir build/msvc                  # unit tests only
    ctest --test-dir build/msvc -L interop       # live tests

The live tests need an endpoint and credentials:

    PSRP_INTEROP=1
    PSRP_CONNECTION=http://host:5985/wsman       # default localhost:5985
    PSRP_USER=Administrator
    PSRP_PASS=...

Ten interop suites run on Linux and six on Windows; the difference is the four
that exercise the curl transport and Kerberos directly. Every suite is
cross-platform except those. `test_features` covers sixteen feature sections
and takes a section name to run just one:

    test_features                 # all sixteen
    test_features hostread        # one

Unit tests prove an encoding matches the specification. They cannot prove a
server accepts it, or that a feature is reachable through the public API at
all, so every feature is also driven against a live PowerShell.

**Fuzzing.** Every parser is fuzzed, deterministically, so a failure
reproduces from its seed:

    scripts\build-asan.bat                                        # ASan build
    ctest -L fuzz
    PSRP_FUZZ_ITERATIONS=1000000 PSRP_FUZZ_SEED=42 fuzz_parsers   # a soak

It reports how often each target accepted its input and fails if any target
never did, since a fuzzer that only exercises rejection paths looks identical
to one that found nothing.

**Differential testing** against `psrpcore`, an independent Python
implementation of the same specification:

    pip install psrpcore
    python tools/differential.py

It runs both directions: psrpcore serializes and we parse, catching an
over-strict reader; we serialize and psrpcore parses, catching a writer only
our own reader would accept. The first direction is baked into a committed
corpus so `ctest` needs neither Python nor psrpcore.

**Lifecycle stress**, for defects that only appear across repetition:

    PSRP_INTEROP=1 PSRP_STRESS_CYCLES=200 test_stress

It runs the open/run/close cycle in a loop and asserts that reusing a
transport does not grow the handle count — process handles on Windows, open
descriptors on Linux.

**Memory.** Every test and a parser soak run under valgrind on Linux in CI,
where any output on stderr also fails the job. AddressSanitizer on Windows has
no leak detector, so the tests and the fuzzer turn on the MSVC debug CRT's
allocation tracking instead; a debug MSVC run reports `(leak-checked)` when it
is active.

## Continuous integration

`.github/workflows/ci.yml` builds and tests on every push and pull request:

- **MSVC** and **clang (llvm-mingw)**, the same two toolchains the local
  scripts drive, so a push cannot land warnings only one compiler sees.
- **Linux**, including the valgrind memcheck job.
- **Interop against a live server.** The Windows runner enables PowerShell
  remoting on itself and the suite connects to `localhost:5985` as the current
  user over Negotiate, so no credential is stored anywhere.
- **Hygiene**, which fails the build on machine-specific configuration in
  tracked files.

`.github/workflows/docs.yml` publishes the API reference to GitHub Pages.

## Build

    scripts\build.bat          # MSVC:  vcvars -> cmake -G Ninja -> ninja -> ctest
    scripts\build-clang.bat    # clang: same, with llvm-mingw

On Linux, CMake with Ninja directly:

    cmake -G Ninja -S . -B build && cmake --build build && ctest --test-dir build

## Documentation

The API reference is published at **https://hugne.github.io/libpsrp/** and
generated from the public headers by Doxygen:

    scripts\build-docs.bat     # -> build\doc\html\index.html

It needs doxygen on PATH (`winget install DimitriVanHeesch.Doxygen`); graphviz
is not used. CI pins doxygen 1.14.0, because versions disagree about what
deserves a warning and the build fails on any warning: comments here are full
of CLIXML element names, and `<b>`, `<c>` and `<s>` are real HTML tags that
doxygen applies to the rendered page rather than printing. Element names in
comments go in backticks.

Only `include/psrp` is scanned. The implementation under `src/` is commented
for people reading it, not for publication.

## Layout

- `include/psrp/` — the public API.
- `src/core/` — buffers, base64, hex, UTF-8/UTF-16, GUIDs.
- `src/proto/` — fragments, message header, CLIXML, typed message bodies.
- `src/state/` — the client state machine (no I/O).
- `src/client/` — the convenience layer, built on the public API.
- `src/transport/` — `winrm_wsman.c` (Win32 WSMan client), `winrm_curl.c`
  (libcurl + GSS-API), `winrm_enumerate.c` (WSMan COM), and
  `psrp_over_winrm.c`, the only file that speaks both vocabularies.
- `src/xml/` — XmlLite and libxml2 behind one pull-parser seam.
- `src/crypto/` — CNG and OpenSSL behind one seam, with the shared half
  factored out.
- `examples/` — `run_command.c` (the protocol, explicitly) and `quick_run.c`
  (the same job through `psrp_client.h`).
- `PLAN.md`, `SPEC-COVERAGE.md`, `TODO.md` — design, per-section coverage, and
  everything deliberately deferred.

## Design

The protocol core performs no I/O. It is a state machine: the caller pumps
received bytes in with `psrp_session_receive`, pulls bytes out with the
payload functions, and reads results from an event queue. The whole protocol
is therefore testable without a network, and the suite drives a full
conversation against a scripted in-memory server.

Backends are selected at compile time behind seams — XML, crypto and
transport — so the platform-specific code is three small files rather than
conditionals through the core. `src/state/` contains no platform or transport
reference at all.

Serialization behaviour is pinned against real PowerShell output rather than
the spec's prose, which is looser than the implementation in several places.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Apache-2.0
rather than MIT because it grants patent rights explicitly, and this
implements a protocol Microsoft holds patents on, published under the Open
Specification Promise.

The protocol itself is Microsoft's. [MS-PSRP] is cited throughout by section
number, which its own IP notice permits; the specification document is not
redistributed here.

[MS-PSRP]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-psrp/
