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
| PSRP-05 | XML backend | 2.2.5 | Portable XML reader to replace XmlLite | XmlLite is the signed-off choice (Microsoft standard, in both toolchains, no vendoring). It is Windows-only; `psrp_xml.h` keeps the seam so a portable parser can be dropped in if we ever leave Windows. Not needed while the transport is Win32 WSMan. | open |
| PSRP-03 | Transport | 2.1 | Non-Windows transport (raw HTTP/SOAP WSMan) | First transport is Win32 WSMan. Core is sans-IO so this is additive. | open |
| PSRP-04 | Crypto | 2.2.5.1.24 | SecureString encryption requires session key exchange | Implemented: RSA key exchange plus AES-256-CBC. The zero IV is inferred, not specified: 2.2.5.1.24 names the algorithm and mode but no IV, and the wire carries nowhere to put one, so a fixed zero IV is the only interoperable reading. Not yet confirmed against a live exchange. | closed |
| PSRP-07 | HostInfo | 2.2.3.14 | Populated `_hostDefaultData` dictionary (colors, coordinates, sizes, window title) | Implemented: all ten required keys with their Color / Coordinates / Size / Int32 / String types. A null host still omits the dictionary entirely, which is what the spec's own null-host example shows. | closed |
| PSRP-08 | Transport | 3.1.5.3.3 | rsp:Command is sent as a single space rather than empty | The spec says the Command element MUST be empty, but the Win32 WSMan client refuses an empty command line client-side (0x80338180) in both WSManRunShellCommand and the Ex form. A single space is the closest achievable and a real PowerShell endpoint accepts it, verified by the live interop test. Only a hand-rolled SOAP transport could send a truly empty element. | open |
| PSRP-09 | Host calls | 2.2.6 | Typed decoding of host method parameters (mp) | Closed: the 2.2.6.1 encodings are implemented. Parameters are reachable by index, T/V wrappers unwrap to their value, plain values pass through unchanged as 2.2.6.1.1 requires, and arrays expose their flattened elements and dimension sizes. What remains undone is mapping each host *method* signature onto typed arguments, which is a job for whatever implements an interactive host, not for the protocol library. | closed |
| PSRP-10 | Metadata | 2.2.3.23 | Full ParameterMetadata detail (type, aliases, switch flag, mandatory) | CommandMetadata surfaces the parameter *names*, which answers the usual question of what a command accepts. Unpacking each parameter's metadata object only matters for a client building tab completion or help, so it waits until something needs it. | open |
| PSRP-11 | Negotiation | 3.1.5.4.1.2 | Version table is not enforced literally | The spec's table requires protocolversion 2.1 or 2.2, PSVersion 2.0 and SerializationVersion 1.1.0.1. Two of those describe no server anyone runs: Windows PowerShell 5.1 announces PSVersion 5.1 and PowerShell 7 announces protocolversion 2.3. Enforcing the table literally would mark every modern server Broken. The check accepts protocolversion major 2 with minor 1 or above, still requires SerializationVersion 1.1.0.1 when present, and ignores PSVersion entirely. Recorded as a deliberate divergence rather than a gap. | open |
| PSRP-06 | Server role | 3.2 | Server-side protocol details | **Out of scope** by sign-off: client only. Recorded here so the exclusion is explicit rather than an oversight. | wontfix |

## Closed

_(none yet)_
