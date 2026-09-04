# libpsrp

A C implementation of the PowerShell Remoting Protocol ([MS-PSRP]), client
side, as a static library with a sans-IO core and a pluggable transport.

**It works against real PowerShell.** The interop test opens a RunspacePool on
a live WinRM endpoint, runs a pipeline, and reads the output back:

```
    pool -> Opened
    server protocolversion 2.2
    running $env:COMPUTERNAME
    pipeline -> Completed
    output: "CLAUDE"
```

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
