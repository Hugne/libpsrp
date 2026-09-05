# TODO / deferred work

Every deferral, approximation, shortcut, or open question found while
implementing goes here. Nothing is dropped silently.

## Where things stand

Every client-side section of MS-PSRP is implemented and tested; see
`SPEC-COVERAGE.md`. The entries still open below are not missing protocol
work. They fall into three groups.

**Deliberate divergences from the spec text** (PSRP-08, PSRP-11, PSRP-13).
Each is a place where following the letter of the specification would break
against real PowerShell, or would lose information a caller needs. Each entry
says what the spec asks, what is done instead, and why. They stay open because
a divergence should keep being visible, not because anything is unfinished.

**A platform defect worked around** (PSRP-14). WSMan leaks a process handle
for every session that does work and is then discarded. It is not ours and
cannot be fixed from here, so both APIs that create a session are shaped to let
callers reuse one, and the entry records the measurements.

**Out of scope for a Windows client** (PSRP-03, PSRP-05). Both exist only to
support a non-Windows port, which would mean replacing the transport and the
XML backend together. Neither affects correctness against a Windows server.
They are recorded so the choice stays visible rather than silently made.

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
| PSRP-01 | Compression | 2.1 | Xpress / MS-XCA compression of WSMan payloads | Closed: nothing to implement. wsman.h documents compression as on by default for Send and Receive, with WSMAN_FLAG_NO_COMPRESSION to turn it off; the transport passes 0 for those flags, so payloads are already compressed by the WSMan client. MS-XCA lives below the PSRP layer, and a hand-rolled HTTP transport (PSRP-03) would be the only thing that had to implement it. | closed |
| PSRP-05 | XML backend | 2.2.5 | Portable XML reader to replace XmlLite | XmlLite is the signed-off choice (Microsoft standard, in both toolchains, no vendoring). It is Windows-only; `psrp_xml.h` keeps the seam so a portable parser can be dropped in if we ever leave Windows. Not needed while the transport is Win32 WSMan. | open |
| PSRP-03 | Transport | 2.1 | Non-Windows transport (raw HTTP/SOAP WSMan) | First transport is Win32 WSMan. Core is sans-IO so this is additive. | open |
| PSRP-04 | Crypto | 2.2.5.1.24 | SecureString encryption requires session key exchange | Implemented: RSA key exchange plus AES-256-CBC. The zero IV is inferred, not specified: 2.2.5.1.24 names the algorithm and mode but no IV, and the wire carries nowhere to put one, so a fixed zero IV is the only interoperable reading. Not yet confirmed against a live exchange. | closed |
| PSRP-07 | HostInfo | 2.2.3.14 | Populated `_hostDefaultData` dictionary (colors, coordinates, sizes, window title) | Implemented: all ten required keys with their Color / Coordinates / Size / Int32 / String types. A null host still omits the dictionary entirely, which is what the spec's own null-host example shows. | closed |
| PSRP-08 | Transport | 3.1.5.3.3 | rsp:Command is sent as a single space rather than empty | The spec says the Command element MUST be empty, but the Win32 WSMan client refuses an empty command line client-side (0x80338180) in both WSManRunShellCommand and the Ex form. A single space is the closest achievable and a real PowerShell endpoint accepts it, verified by the live interop test. Only a hand-rolled SOAP transport could send a truly empty element. | open |
| PSRP-09 | Host calls | 2.2.6 | Typed decoding of host method parameters (mp) | Closed: the 2.2.6.1 encodings are implemented. Parameters are reachable by index, T/V wrappers unwrap to their value, plain values pass through unchanged as 2.2.6.1.1 requires, and arrays expose their flattened elements and dimension sizes. What remains undone is mapping each host *method* signature onto typed arguments, which is a job for whatever implements an interactive host, not for the protocol library. | closed |
| PSRP-10 | Metadata | 2.2.3.23 | Full ParameterMetadata detail (type, aliases, switch flag, mandatory) | Closed: each parameter's type name, aliases, switch flag and dynamic flag are read alongside its name, index-aligned with the names array so a caller can walk both together. A parameter whose metadata object is missing still gets an entry carrying its name, so one odd parameter does not cost the caller the rest. There is no Mandatory property in 2.2.3.23; it belongs to the parameter *set* metadata, which the spec does not define a wire type for. | closed |
| PSRP-11 | Negotiation | 3.1.5.4.1.2 | Version table is not enforced literally | The spec's table requires protocolversion 2.1 or 2.2, PSVersion 2.0 and SerializationVersion 1.1.0.1. Two of those describe no server anyone runs: Windows PowerShell 5.1 announces PSVersion 5.1 and PowerShell 7 announces protocolversion 2.3. Enforcing the table literally would mark every modern server Broken. The check accepts protocolversion major 2 with minor 1 or above, still requires SerializationVersion 1.1.0.1 when present, and ignores PSVersion entirely. Recorded as a deliberate divergence rather than a gap. | open |
| PSRP-12 | Discovery | 3.1.4.10.1 | Enumerating disconnected shells and commands on a server | Closed: implemented via IWSManSession::Enumerate. The earlier note here was wrong. The flat WSMan C API has no enumerate entry point, but the WSMan *automation* interface does, and it is the same one `winrm enumerate` uses. Live-verified: the enumeration lists our own open pool with a matching ShellId. Supplying credentials needs WSManFlagCredUsernamePassword, which the C API infers from the authentication struct and the automation API makes you state. | closed |
| PSRP-13 | Error handling | 3.1.7 | An error marks the pool Broken rather than Closed | 3.1.7 says an error while processing a RunspacePool message closes that pool. This uses Broken. Closed everywhere else in the spec means an orderly shutdown, so reporting a failed pool as Closed would leave a caller who looks at the state unable to tell a decode error from a clean exit. Both states stop further processing identically under 3.1.5.1 rule 5, so nothing else changes. Recorded as a deliberate divergence. | open |
| PSRP-14 | Transport | 3.1.4.10.1, 2.1 | WSMan leaks a handle per discarded session | Not our leak and not fixable from here: a WSMan session that performs work and is then released costs about one process handle, and CoUninitialize does not reclaim it. It reproduces in a standalone program touching none of this library's code. A session created but never used does not leak, and a reused one does not either. It affects both entry points that create a session: shell discovery, mitigated by psrp_discovery_t, and the transport, mitigated by reusing a psrp_transport_t across shells. Measured: 100 one-shot enumerations +112 handles against 100 through one handle +1; a new transport per shell +1.15 each against 80 shells on one transport +5 then flat. Both one-shot forms still pay it once per call and say so. | open |
| PSRP-15 | Transport | 3.1.5.3.7 | Intermittent failure when cycling transports in a tight loop | Closed: root-caused to a race in the Receive cancellation. Retargeting Receive cancels the in-flight operation, which reports ERROR_OPERATION_ABORTED, and a single expect_abort flag marked that as ours. But cancellation is asynchronous: the completion can arrive after the replacement Receive has been posted and the flag cleared, and the abort is then recorded as a server fault. Replaced with a count of outstanding cancellations that only a consuming callback decrements, and which the receive reset deliberately leaves alone. Reproduced at cycles 41, 48 and 72 before the fix; 400 consecutive cycles clean after it. Guarded by tests/interop/test_stress.c. | closed |
| PSRP-16 | Pipeline input | 2.2.2.10 | NoInput was always true, so sent input was silently discarded | Closed: psrp_session_pipeline_payload used the CREATE_PIPELINE defaults unconditionally, and those set NoInput=true. A server told that closes the input stream on receipt, so psrp_session_send_input could never work through the session's own pipeline creation: the two public functions were unusable together. The failure was silent -- the script just saw an empty $input. Callers now state their intent with PSRP_PIPELINE_EXPECT_INPUT or PSRP_PIPELINE_NO_INPUT. Found only because the input direction was finally run against a live server. | closed |
| PSRP-06 | Server role | 3.2 | Server-side protocol details | **Out of scope** by sign-off: client only. Recorded here so the exclusion is explicit rather than an oversight. | wontfix |

## Closed

_(none yet)_
