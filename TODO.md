# TODO / deferred work

Every deferral, approximation, shortcut, or open question found while
implementing goes here. Nothing is dropped silently.

## Rules

- Each entry gets a stable id `PSRP-nn`. Code references it as `/* TODO(PSRP-nn) */`.
- An entry records: what, which spec section, why deferred, and what "done" means.
- Entries are only closed when the behaviour is implemented **and** tested.
- **Before declaring any phase or the project done**, re-read this file top to
  bottom and reconcile it against `SPEC-COVERAGE.md`. Any `deferred` row in the
  coverage file must have a matching open entry here, and vice versa.

## Status legend

`open` — not started · `wip` — in progress · `closed` — done + tested · `wontfix` — agreed out of scope

---

## Open

| id | Area | Spec | Item | Why deferred | Status |
|---|---|---|---|---|---|
| PSRP-01 | Compression | 2.1 | Xpress / MS-XCA compression of WSMan payloads | Optional on the wire; correctness does not depend on it. Revisit if a server demands it. | open |
| PSRP-02 | Server role | 3.2 | Server-side protocol details | Client-first by decision; pending sign-off on whether server is in scope at all. | open |
| PSRP-03 | Transport | 2.1 | Non-Windows transport (raw HTTP/SOAP WSMan) | First transport is Win32 WSMan. Core is sans-IO so this is additive. | open |
| PSRP-04 | Crypto | 2.2.5.1.24 | SecureString encryption requires session key exchange | Scheduled phase 9; until then SecureString round-trips only as an opaque/rejected value. | open |

## Closed

_(none yet)_
