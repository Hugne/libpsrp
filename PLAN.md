# libpsrp — a C implementation of MS-PSRP

A static C library implementing the PowerShell Remoting Protocol ([MS-PSRP]),
built incrementally with full spec coverage as the goal.

Status: **awaiting sign-off**. No implementation code written yet.

---

## 1. Goals and non-goals

**Goals**

- C11 static library (`psrp.lib`) with a small, stable, well-documented public API.
- Cover the whole of [MS-PSRP], tracked section-by-section in `SPEC-COVERAGE.md`.
- Build with CMake + Ninja. Test with CTest.
- Grow in reviewable increments; every phase builds, tests, and is committed.
- Anything skipped is recorded in `TODO.md` — never silently dropped.

**Non-goals (for now, see Open Questions)**

- Not a PowerShell engine. We speak the protocol; the remote end runs the code.
- Server side (spec section 3.2) is tracked but scheduled last.
- No cross-platform transport initially; the first transport is Win32 WSMan.

---

## 2. Architecture: sans-IO core

The single most important design decision: **the protocol core performs no I/O.**
It is a pure state machine — bytes and events in, bytes and events out. Transport,
crypto, and host callbacks are injected through vtables.

This is the same approach `psrpcore` takes, and it buys us:

- Unit tests that need no network: feed canned server bytes, assert client bytes.
- Deterministic, reproducible tests for a protocol that is otherwise async.
- Freedom to add transports (WSMan, raw HTTP/SOAP, in-process mock) later.

```
   application
        |  psrp_session / psrp_pipeline  (public API)
        v
 +-------------------------------------------+
 |  state:  runspace pool + pipeline FSMs    |
 |  proto:  messages  <->  CLIXML  <->  objects
 |  frag :  fragmenter / defragmenter        |   <-- pure, no I/O
 +-------------------------------------------+
        ^ psrp_transport_t        ^ psrp_crypto_t     ^ psrp_host_t
        |                         |                   |
   WSMan (Win32)             CNG (BCrypt)        caller-supplied
   mock (tests)              null (tests)        default no-op
```

### Layer map (bottom to top)

| Layer | Spec | Module |
|---|---|---|
| Byte buffers, base64, hex, UTF-8, GUID | — | `src/core/` |
| Packet fragments | 2.2.4 | `src/proto/fragment.c` |
| Message header + 31 message types | 2.2.1, 2.2.2 | `src/proto/message.c` |
| CLIXML read/write | 2.2.5 | `src/proto/clixml_*.c` |
| PS object model | 2.2.3, 2.2.5.2 | `src/proto/object.c` |
| Host method calls | 2.2.6 | `src/state/host.c` |
| Runspace pool + pipeline FSMs | 3.1 | `src/state/` |
| Transport backends | 2.1 | `src/transport/` |
| Crypto (session key, SecureString) | 2.2.2.3–5 | `src/crypto/` |

---

## 3. Repository layout

```
libpsrp/
  CMakeLists.txt
  PLAN.md  TODO.md  SPEC-COVERAGE.md  README.md
  include/psrp/          # public API only; no internal headers leak
    psrp.h               # umbrella
    psrp_types.h  psrp_error.h  psrp_buffer.h
    psrp_fragment.h  psrp_message.h
    psrp_object.h  psrp_clixml.h
    psrp_session.h  psrp_pipeline.h
    psrp_transport.h  psrp_crypto.h  psrp_host.h
  src/
    core/  proto/  state/  transport/  crypto/
    internal/            # private headers
  tests/
    unit/                # one ctest target per module
    vectors/             # golden CLIXML fixtures (generated + checked in)
    interop/             # live WinRM tests, tagged, opt-in
  tools/
    gen_vectors.ps1      # produce golden CLIXML from real PowerShell
  reference/doc/
    MS-PSRP.pdf          # v25.0 spec, 184 pages
    MS-PSRP.txt          # extracted text, greppable during implementation
```

---

## 4. Build

- **C11**, warnings-as-errors (`/W4 /WX` MSVC, `-Wall -Wextra -Werror` clang).
- **CMake ≥ 3.20**, **Ninja** generator. Validated on this box: MSVC 19.51 builds
  a static lib under Ninja.
- Primary toolchain **MSVC** (Build Tools 18 via `vcvars64.bat`).
  Secondary **llvm-mingw clang** — it also ships `wsman.h`, so both can build the
  WSMan transport. Core + tests are portable C and must build under both.
- `scripts/build.bat` wraps vcvars → cmake configure → ninja → ctest.

---

## 5. Dependencies (deliberately few)

| Need | Decision | Why |
|---|---|---|
| XML parsing | **expat** (MIT), vendored, behind `psrp_xml.h` | CLIXML reading is where hand-rolled parsers go to die. Battle-tested, streaming, MIT. The interface lets us swap it. |
| XML writing | in-repo | We only emit a constrained subset; trivial and avoids a dep. |
| Base64 / hex / UTF-8 | in-repo | ~150 lines total, no dep worth taking. |
| Crypto (RSA, AES, RNG) | **Windows CNG (BCrypt)** behind `psrp_crypto.h` | Already available, no external dep. OpenSSL/mbedTLS can slot in later. |
| Transport | **Win32 WSMan** (`wsman.h`) behind `psrp_transport.h` | We already have working WSMan shell code in `../winrm` to adapt. |
| Unit test harness | in-repo `tests/unit/harness.h` (~100 lines) + CTest | Zero deps, full control, trivial CTest registration. `utest.h` (MIT) is the fallback if we outgrow it. |
| Xpress/MS-XCA compression | **deferred** → TODO | Optional on the wire; not needed for correctness. |

No external reusable CLIXML implementation in C exists — the research found none.
The reference implementations are `psrpcore`/`pypsrp` (Python) and PowerShell
itself (C#). We use them as **behavioural oracles**, not as code to link.

---

## 6. Testing strategy (CTest)

CTest is a good fit: it is the natural CMake runner, gives us labels, timeouts,
parallelism, and per-test isolation. It runs plain executables, so the harness
stays tiny. Five tiers:

1. **Unit tests** — one target per module (`test_fragment`, `test_clixml`, …).
   Registered with `add_test`, labelled `unit`.
2. **Round-trip property tests** — encode→decode→compare for fragments,
   messages, and every CLIXML type. Catches asymmetry bugs cheaply.
3. **Golden vectors** — `tools/gen_vectors.ps1` runs real PowerShell
   (`[System.Management.Automation.PSSerializer]::Serialize`) over a corpus of
   types and checks in the CLIXML. Our deserializer must parse all of it; our
   serializer must produce semantically equal output. This is ground truth from
   the actual implementation we must interoperate with — a big win, and we have
   PowerShell right here to generate it.
4. **Differential tests** — optional, `pip install psrpcore`; cross-check our
   fragment/message bytes against a second independent implementation.
5. **Interop tests** — labelled `interop`, opt-in via `PSRP_INTEROP=1`. Drive a
   real runspace pool against `localhost` WinRM and assert
   `$env:COMPUTERNAME`. This is the acceptance test for "it actually works".
   Excluded from the default run so unit tests need no credentials or service.

Later: libFuzzer targets for the fragment and CLIXML parsers (untrusted input),
plus ASan/UBSan builds under clang.

---

## 7. Phase plan

Each phase ends green: builds clean, tests pass, `SPEC-COVERAGE.md` updated,
committed. Phases 0–6 are the critical path to a working client.

| # | Phase | Spec | Exit criteria |
|---|---|---|---|
| 0 | Scaffold: CMake/Ninja/CTest, error model, buffers, base64/hex/GUID/UTF-8 | — | `ctest` green with utility tests |
| 1 | Fragment layer | 2.2.4 | Fragment/defragment round-trip; malformed input rejected, never crashes |
| 2 | Message header + all 31 type codes | 2.2.1, 2.2.2 | Header pack/unpack round-trip; type enum complete |
| 3a | CLIXML: primitives | 2.2.5.1 (26 types) | All primitives round-trip; golden vectors parse |
| 3b | CLIXML: complex objects, containers, RefId graph | 2.2.5.2 | Lists/dicts/stacks/queues/enums/refs; cyclic graphs safe |
| 3c | CLIXML: string encoding, property sets, ToString, type names | 2.2.5.3 | `_xNNNN_` escaping exact; full golden corpus green |
| 4 | Typed bodies for the "run a command" message set | 2.2.2.1/2/9/10/19/20/21 | Build and parse each from/to objects |
| 5 | Runspace pool + pipeline state machines (sans-IO) | 3.1 | Canned-byte-stream tests drive open→run→output→close |
| 6 | **WSMan transport + first live run** | 2.1 | Interop test opens a pool on localhost and returns the computer name |
| 7 | Remaining streams: debug/verbose/warning/progress/information, pipeline input | 2.2.2.17/18/22–26 | Each stream surfaces through the public API |
| 8 | Host method calls + host interface | 2.2.6, 2.2.2.15/16/27/28 | `Write-Host`/prompt round-trip to a caller-supplied host |
| 9 | Crypto: public key exchange, SecureString | 2.2.2.3/4/5, 2.2.5.1.24 | PSCredential survives a round trip to a real endpoint |
| 10 | Remaining messages: min/max runspaces, availability, user event, app private data, command metadata, connect/reconnect, reset | 2.2.2.6–8/11–14/29–31 | `SPEC-COVERAGE.md` has no `todo` in section 2 |
| 11 | Hardening: fuzzing, sanitizers, leak checks, API docs, examples | — | Fuzzers run clean; public headers documented |
| 12 | Server side (if in scope) | 3.2 | Decide at sign-off — see Open Questions |

---

## 8. Working rules for the autonomous run

1. **Spec first.** Before implementing a section, grep `reference/doc/MS-PSRP.txt`
   and cite the section number in a comment above the code.
2. **Never silently skip.** Anything deferred, approximated, or unclear gets a
   `TODO.md` entry with an id, the spec section, and the reason. Code carries
   `/* TODO(PSRP-nn) */` referencing it.
3. **Update `SPEC-COVERAGE.md` in the same commit** as the code it describes.
4. **Definition of done checklist** (run before claiming any phase complete):
   build clean both toolchains → `ctest` green → coverage file updated →
   TODO file reconciled → committed.
5. **Small commits**, one logical step each, imperative subject lines.
6. If a spec detail is genuinely ambiguous, implement what real PowerShell does
   (verified with a golden vector) and note the divergence in `TODO.md`.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| CLIXML is the bulk of the work and easy to get subtly wrong | Golden vectors from real PowerShell, generated early (phase 3a), not hand-written |
| Async WSMan + protocol state machine is hard to debug together | Sans-IO core: all protocol logic tested without any transport |
| Interop failures are opaque (server just closes) | Wire-level tracing hook + hex dump of every fragment behind a debug flag; capture a known-good exchange from PowerShell early |
| Scope creep to a full PowerShell object system | Public API exposes a deliberately small object model; richer typing only where the spec needs it |
| Non-admin WinRM access | Already solved: credentials work against localhost; interop tests are opt-in anyway |

---

## 10. Open questions for sign-off

1. **Client only, or client + server?** Full spec includes server (3.2). I
   recommend client-first, server as phase 12 only if you want it. It roughly
   doubles the work.
2. **expat vendored, or hand-rolled XML reader?** I recommend expat (MIT) for
   correctness; hand-rolling is the main "subtle bugs forever" risk.
3. **Test harness**: tiny in-repo harness (recommended, zero deps) vs vendoring
   `utest.h`.
4. **Toolchains**: keep both MSVC and clang green, or MSVC only? Dual is a small
   tax and catches real portability bugs early.
5. **May I `pip install psrpcore`** for differential testing, and vendor expat
   (both need one-time network fetches)?
6. **Library name / API prefix**: `libpsrp` / `psrp_*`. Happy to change.
