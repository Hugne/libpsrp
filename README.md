# libpsrp

A C implementation of the PowerShell Remoting Protocol ([MS-PSRP]) as a static
library, with a sans-IO core and pluggable transport, crypto, and host callbacks.

- `PLAN.md` — architecture, phase plan, and open questions.
- `SPEC-COVERAGE.md` — per-section spec coverage (the definition of "done").
- `TODO.md` — everything deferred, with reasons.
- `reference/doc/` — the [MS-PSRP] specification (PDF + extracted text).

Status: planning. No implementation code yet.

## Build (once implemented)

    scripts\build.bat        # vcvars -> cmake -G Ninja -> ninja -> ctest

## Testing

CTest drives unit, round-trip, golden-vector, and (opt-in) live interop tests.
Interop tests need `PSRP_INTEROP=1` and WinRM credentials; they are excluded
from the default run.
