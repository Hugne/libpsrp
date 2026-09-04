# MS-PSRP Spec Coverage

Tracks implementation + test status for every section of [MS-PSRP].

Status: `todo` | `wip` | `done` | `deferred` (deferred entries MUST have a TODO.md id).

This file is the definition of "full spec coverage". Update it in the same commit as the code.

**Scope: client side only.** Server sections (3.2) are marked `n/a` by design.

### Infrastructure (no spec section)

| Component | Impl | Tests | Notes |
|---|---|---|---|
| Result codes / error strings | done | done | `psrp_error.h` |
| Byte buffer + reader (big-endian) | done | done | `psrp_buffer.h` |
| GUID parse/format/wire layout | done | done | .NET little-endian field order verified |
| base64 / hex | done | done | RFC 4648 vectors |
| UTF-8 validation, UTF-8<->UTF-16 | done | done | needed for XmlLite (UTF-16) |
| Build: CMake+Ninja, MSVC & clang, CTest | done | done | both green, warnings-as-errors |


## Message Layer

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.1 | PowerShell Remoting Protocol Message | done | done | 40-byte header, little-endian per 2.2; GUIDs in .NET field order; unknown types preserved |
| 2.2.4 | Packet Fragment | done | done | encode/decode/split + streaming defragmenter; reserved bits ignored on receipt |

## Message Types (2.2.2)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.2 | Message Types | wip | wip | all 31 type codes done + verified; per-message Data bodies need CLIXML (2.2.5) |
| 2.2.2.1 | SESSION_CAPABILITY Message | done | done | build + parse; TimeZone ignored (PSRP does not interpret it) |
| 2.2.2.2 | INIT_RUNSPACEPOOL Message | done | done | build; ApplicationArguments sent as Null |
| 2.2.2.3 | PUBLIC_KEY Message | done | done | build; 276-byte CryptoAPI PUBLICKEYBLOB, little-endian |
| 2.2.2.4 | ENCRYPTED_SESSION_KEY Message | done | done | parse SIMPLEBLOB and import the AES-256 key |
| 2.2.2.5 | PUBLIC_KEY_REQUEST Message | done | done | build and recognise the empty string payload |
| 2.2.2.6 | SET_MAX_RUNSPACES Message | done | done | build; ci is a Signed Long |
| 2.2.2.7 | SET_MIN_RUNSPACES Message | done | done | build; ci is a Signed Long |
| 2.2.2.8 | RUNSPACE_AVAILABILITY Message | done | done | parse; response is Boolean or Signed Long depending on the request |
| 2.2.2.9 | RUNSPACEPOOL_STATE Message | done | done | parse incl. optional ExceptionAsErrorRecord text |
| 2.2.2.10 | CREATE_PIPELINE Message | done | done | build, multi-command pipelines with named + positional parameters |
| 2.2.2.11 | GET_AVAILABLE_RUNSPACES Message | done | done | build |
| 2.2.2.12 | USER_EVENT Message | done | done | parse; surfaced as its own session event |
| 2.2.2.13 | APPLICATION_PRIVATE_DATA Message | done | done | surfaced as an opaque object; PSRP does not interpret it |
| 2.2.2.14 | GET_COMMAND_METADATA Message | done | done | build; Null Name means "*" per spec |
| 2.2.2.15 | RUNSPACEPOOL_HOST_CALL Message | done | done | parse to ci/mi/mp |
| 2.2.2.16 | RUNSPACEPOOL_HOST_RESPONSE Message | done | done | build with mr or me |
| 2.2.2.17 | PIPELINE_INPUT Message | done | done | serialize any value |
| 2.2.2.18 | END_OF_PIPELINE_INPUT Message | done | done | empty Data field, per spec |
| 2.2.2.19 | PIPELINE_OUTPUT Message | done | done | deserialize any value + text rendering |
| 2.2.2.20 | ERROR_RECORD Message | done | done | parse to typed fields |
| 2.2.2.21 | PIPELINE_STATE Message | done | done | parse incl. optional ExceptionAsErrorRecord text |
| 2.2.2.22 | DEBUG_RECORD Message | done | done | informational record parser |
| 2.2.2.23 | VERBOSE_RECORD Message | done | done | informational record parser |
| 2.2.2.24 | WARNING_RECORD Message | done | done | informational record parser |
| 2.2.2.25 | PROGRESS_RECORD Message | done | done | parse to typed fields |
| 2.2.2.26 | INFORMATION_RECORD Message | done | done | parse; properties arrive as adapted <Props> |
| 2.2.2.27 | PIPELINE_HOST_CALL Message | done | done | parse to ci/mi/mp |
| 2.2.2.28 | PIPELINE_HOST_RESPONSE Message | done | done | build with mr or me |
| 2.2.2.29 | CONNECT_RUNSPACEPOOL Message | done | done | build; both bounds optional, empty form supported |
| 2.2.2.30 | RUNSPACEPOOL_INIT_DATA Message | done | done | parse; absent bounds reported as -1 |
| 2.2.2.31 | RESET_RUNSPACE_STATE Message | done | done | build |

## Other Object Types (2.2.3)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.3 | Other Object Types | todo | todo | |
| 2.2.3.1 | Coordinates | done | done | T/V wrapper, build + read |
| 2.2.3.2 | Size | done | done | T/V wrapper, build + read |
| 2.2.3.3 | Color | done | done | T/V wrapper; 0 accepted as Black though the spec table omits it |
| 2.2.3.4 | RunspacePoolState | done | done | all 10 values + terminal-state helper |
| 2.2.3.5 | PSInvocationState | done | done | all 7 values + terminal-state helper |
| 2.2.3.6 | PSThreadOptions | done | done | enum object with type names + ToString |
| 2.2.3.7 | ApartmentState | done | done | enum object with type names + ToString |
| 2.2.3.8 | RemoteStreamOptions | done | done | bit flags defined; sent in CREATE_PIPELINE |
| 2.2.3.9 | ErrorCategory | done | done | names for 0-22 and 25 (23/24 undefined by spec) |
| 2.2.3.10 | TimeZone | todo | todo | |
| 2.2.3.10.1 | CurrentSystemTimeZone | todo | todo | |
| 2.2.3.10.2 | Hashtable from int to DaylightTime Using Default Comparer | todo | todo | |
| 2.2.3.10.3 | DaylightTime | todo | todo | |
| 2.2.3.11 | Pipeline | done | done | Cmds/IsNested/History/RedirectShellErrorOutputPipe |
| 2.2.3.12 | Command | done | done | Cmd/IsScript/UseLocalScope/merge flags/Args |
| 2.2.3.13 | Command Parameter | done | done | named and positional (Null N) |
| 2.2.3.14 | HostInfo | done | done | null-host and populated _hostDefaultData with all ten required keys |
| 2.2.3.15 | ErrorRecord | done | done | message, error id, category fields |
| 2.2.3.15.1 | InvocationInfo-specific Extended Properties | todo | todo | |
| 2.2.3.16 | InformationalRecord (DebugRecord, WarningRecord or VerboseRecord) | done | done | message + invocation-info flag |
| 2.2.3.17 | Host Method Identifier | done | done | all 56 methods, with the returns-a-value rule |
| 2.2.3.18 | Primitive Dictionary | todo | todo | |
| 2.2.3.19 | CommandType | done | done | bit flags; well-known CommandTypes values named |
| 2.2.3.20 | Wildcard | done | done | a String; backtick escapes are the caller's to write |
| 2.2.3.21 | CommandMetadataCount | done | done | parse Count |
| 2.2.3.22 | CommandMetadata | done | done | parse name, namespace, help uri, type, parameter names |
| 2.2.3.23 | ParameterMetadata | wip | wip | parameter names surfaced; per-parameter detail deferred (TODO PSRP-10) |
| 2.2.3.24 | ArgumentList | todo | todo | |
| 2.2.3.25 | PSCredential | todo | todo | |
| 2.2.3.26 | KeyInfo | todo | todo | |
| 2.2.3.27 | ControlKeyStates | todo | todo | |
| 2.2.3.28 | BufferCell | todo | todo | |
| 2.2.3.29 | BufferCellType | todo | todo | |
| 2.2.3.30 | CommandOrigin | todo | todo | |
| 2.2.3.31 | PipelineResultTypes | done | done | bit flags defined |

## Serialization (2.2.5)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.5 | Serialization | done | done | writer + reader, XmlLite backend, round-trip tested |
| 2.2.5.1 | Serialization of Primitive Type Objects | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.1 | String | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.2 | Character | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.3 | Boolean | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.4 | Date/Time | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.5 | Duration | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.6 | Unsigned Byte | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.7 | Signed Byte | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.8 | Unsigned Short | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.9 | Signed Short | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.10 | Unsigned Int | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.11 | Signed Int | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.12 | Unsigned Long | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.13 | Signed Long | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.14 | Float | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.15 | Double | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.16 | Decimal | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.17 | Array of Bytes | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.18 | GUID | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.19 | URI | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.20 | Null Value | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.21 | Version | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.22 | XML Document | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.23 | ScriptBlock | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.1.24 | Secure String | done | done | AES-256-CBC under the session key, zero IV, UTF-16LE plaintext |
| 2.2.5.1.25 | Progress Record | done | done | parse to typed fields |
| 2.2.5.1.26 | Information Record | done | done | parse to typed fields |
| 2.2.5.2 | Serialization of Complex Objects | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.1 | Referencing Earlier Objects | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.1.1 | RefId Attribute | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.1.2 | <Ref> Element | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.2 | <Obj> Element | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.3 | Type Names | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.4 | ToString | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.5 | Contents of Extended Primitive Objects | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.6 | Contents of Known Containers | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.6.1 | Stack | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.6.2 | Queue | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.6.3 | List | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.6.4 | Dictionaries | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.7 | Contents of Enums | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.8 | Adapted Properties | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.2.9 | Extended Properties | done | done | serialize + deserialize, round-trip tested |
| 2.2.5.3 | Miscellaneous | todo | todo | |
| 2.2.5.3.1 | Property Name | wip | wip | N= attribute written and escaped; read pending |
| 2.2.5.3.2 | Encoding Strings | done | done | matches real PowerShell, incl. underscore-before-x rule and surrogate escapes |
| 2.2.5.3.3 | Lifetime of a Serializer/Deserializer Pair | todo | todo | |
| 2.2.5.3.4 | Structure of Complex Objects | todo | todo | |
| 2.2.5.3.4.1 | Adapted Properties | todo | todo | |
| 2.2.5.3.4.2 | Extended Properties | todo | todo | |
| 2.2.5.3.4.3 | Property Sets | todo | todo | |
| 2.2.5.3.4.4 | ToString Value | todo | todo | |
| 2.2.5.3.4.5 | Type Names | todo | todo | |

## Host Method Calls (2.2.6)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.6 | Encoding Host Parameters in Host Method Calls | wip | wip | mp surfaced as a parsed object; typed per-method decoding deferred (TODO PSRP-09) |
| 2.2.6.1 | Encoding Individual Parameters | todo | todo | |
| 2.2.6.1.1 | Any Serializable Type | todo | todo | |
| 2.2.6.1.2 | CultureInfo | todo | todo | |
| 2.2.6.1.3 | List | todo | todo | |
| 2.2.6.1.4 | Array | todo | todo | |
| 2.2.6.1.5 | Collection | todo | todo | |
| 2.2.6.1.6 | Dictionary | todo | todo | |
| 2.2.6.1.7 | Object Dictionary | todo | todo | |
| 2.2.6.1.8 | Other Object Types Used in a Host Call | todo | todo | |

## Client Protocol Details (3.1)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 3.1 | Client Details | wip | wip | session state machine (sans-IO) done; host calls, key exchange, disconnect/reconnect pending |
| 3.1.1 | Abstract Data Model | wip | wip | pool + pipeline data modelled; session key and CI tables pending |
| 3.1.1.1 | Global Data | todo | todo | |
| 3.1.1.1.1 | WSMV ShellID to RunspacePool Table | todo | todo | |
| 3.1.1.1.2 | WSMV CommandId to Pipeline Table | todo | todo | |
| 3.1.1.1.3 | Public Key Pair | done | done | 2048-bit RSA generated per crypto context |
| 3.1.1.2 | RunspacePool Data | wip | wip | id/state/defrag done; session key + information tables pending |
| 3.1.1.2.1 | GUID | done | done | pool id, random v4 from the platform CSPRNG |
| 3.1.1.2.2 | RunspacePool State | done | done | tracked and exposed |
| 3.1.1.2.3 | Defragmentation Data | done | done | per-session defragmenter |
| 3.1.1.2.4 | WSMV Shell | todo | todo | |
| 3.1.1.2.5 | RunspacePool Information CI Table | todo | todo | |
| 3.1.1.2.6 | Pipeline Table | todo | todo | |
| 3.1.1.2.7 | Session Key | done | done | held by the crypto context once exchanged |
| 3.1.1.2.8 | SessionKeyTransferTimeoutms | todo | todo | |
| 3.1.1.3 | Pipeline Data | wip | wip | id/state/defrag done; WSMV command handle is the transport's |
| 3.1.1.3.1 | GUID | done | done | pipeline id allocated per pipeline |
| 3.1.1.3.2 | Pipeline State | done | done | surfaced as an event |
| 3.1.1.3.3 | Defragmentation Data | done | done | shared session defragmenter |
| 3.1.1.3.4 | WSMV Command | todo | todo | |
| 3.1.2 | Timers | todo | todo | |
| 3.1.3 | Initialization | done | done | session construction; pool id generated |
| 3.1.4 | Higher-Layer Triggered Events | wip | wip | create pool + execute pipeline done; the rest pending |
| 3.1.4.1 | Creating a RunspacePool | done | done | open payload: SESSION_CAPABILITY + INIT_RUNSPACEPOOL |
| 3.1.4.2 | Closing a RunspacePool | done | done | explicit close; state tracked |
| 3.1.4.3 | Executing a Pipeline | done | done | CREATE_PIPELINE payload + pipeline id |
| 3.1.4.4 | Stopping a Pipeline | done | done | pipeline state tracked; signal handled by the transport |
| 3.1.4.5 | Getting Command Metadata | done | done | request builder + result stream parsers |
| 3.1.4.6 | Setting the Minimum or Maximum Runspaces in a RunspacePool | done | done | builders + availability response |
| 3.1.4.7 | Getting the Number of Available Runspaces in a RunspacePool | done | done | builder + availability response |
| 3.1.4.8 | Initiating a Session Key Exchange | done | done | public key export + encrypted session key import |
| 3.1.4.9 | Disconnecting from a RunspacePool | todo | todo | |
| 3.1.4.10 | Connecting to a RunspacePool | todo | todo | |
| 3.1.4.10.2 | Connecting to a RunspacePool from a Previous Client Session | todo | todo | |
| 3.1.4.10.3 | Connecting to a RunspacePool from a New Client Session | todo | todo | |
| 3.1.5 | Message Processing Events and Sequencing Rules | done | done | send/receive rules, WSMan binding, and sequencing verified live |
| 3.1.5.1 | General Rules | todo | todo | |
| 3.1.5.1.1 | Rules for Sending Data | done | done | message framing + fragmentation on send |
| 3.1.5.1.2 | Rules for Receiving Data | done | done | defragment, decode, dispatch to events |
| 3.1.5.2 | Sequencing Rules | done | done | capability first, then init; pool must be Opened before a pipeline |
| 3.1.5.3 | Rules for Processing WS-MAN Messages | done | done | Create/Command/Send/Receive implemented |
| 3.1.5.3.1 | Rules for the wxf:Create Message | done | done | creationXml + protocolversion option + stdin/pr/stdout streams |
| 3.1.5.3.2 | Rules for the wxf:ResourceCreated Message | done | done | shell handle tracked by the WSMan client |
| 3.1.5.3.3 | Rules for the wxf:Command Message | done | done | first fragment in Arguments, remainder via Send (see TODO PSRP-08) |
| 3.1.5.3.4 | Rules for the wxf:CommandResponse Message | done | done | command handle tracked by the WSMan client |
| 3.1.5.3.5 | Rules for the wxf:Send Message | done | done | stdin stream |
| 3.1.5.3.6 | Rules for the wxf:SendResponse Message | done | done | completion handled by the WSMan client |
| 3.1.5.3.7 | Rules for the wxf:Receive Message | done | done | stdout stream, re-posted until the command is Done |
| 3.1.5.3.8 | Rules for the wxf:ReceiveResponse Message | done | done | stream data fed to the session |
| 3.1.5.3.9 | Rules for the wxf:Signal Message | done | done | stop pipeline; code is the spec's misspelled powershell/signal/crtl_c |
| 3.1.5.3.10 | Rules for the wxf:SignalResponse Message | done | done | completion handled by the WSMan client |
| 3.1.5.3.11 | Rules for the wxf:Delete Message | done | done | explicit shell close, verified live |
| 3.1.5.3.12 | Rules for the wxf:DeleteResponse Message | done | done | completion handled by the WSMan client |
| 3.1.5.3.13 | Rules for the wxf:Fault Message | todo | todo | |
| 3.1.5.3.14 | Rules for the wxf:Connect Message | todo | todo | |
| 3.1.5.3.15 | Rules for the wxf:ConnectResponse Message | todo | todo | |
| 3.1.5.3.16 | Rules for the wxf:Disconnect Message | todo | todo | |
| 3.1.5.3.17 | Rules for the wxf:DisconnectResponse Message | todo | todo | |
| 3.1.5.3.18 | Rules for the wxf:Reconnect Message | todo | todo | |
| 3.1.5.3.19 | Rules for the wxf:ReconnectResponse Message | todo | todo | |
| 3.1.5.4 | Rules for Processing PSRP Messages | todo | todo | |
| 3.1.5.4.1 | SESSION_CAPABILITY Message | todo | todo | |
| 3.1.5.4.1.1 | Sending to the Server | todo | todo | |
| 3.1.5.4.1.2 | Receiving from the Server | todo | todo | |
| 3.1.5.4.2 | INIT_RUNSPACEPOOL Message | todo | todo | |
| 3.1.5.4.3 | PUBLIC_KEY Message | todo | todo | |
| 3.1.5.4.4 | ENCRYPTED_SESSION_KEY Message | todo | todo | |
| 3.1.5.4.5 | PUBLIC_KEY_REQUEST Message | todo | todo | |
| 3.1.5.4.6 | SET_MAX_RUNSPACES Message | todo | todo | |
| 3.1.5.4.7 | SET_MIN_RUNSPACES Message | todo | todo | |
| 3.1.5.4.8 | RUNSPACE_AVAILABILITY Message | todo | todo | |
| 3.1.5.4.9 | RUNSPACEPOOL_STATE Message | todo | todo | |
| 3.1.5.4.10 | CREATE_PIPELINE Message | todo | todo | |
| 3.1.5.4.11 | GET_AVAILABLE_RUNSPACES Message | todo | todo | |
| 3.1.5.4.12 | USER_EVENT Message | todo | todo | |
| 3.1.5.4.13 | APPLICATION_PRIVATE_DATA Message | todo | todo | |
| 3.1.5.4.14 | GET_COMMAND_METADATA Message | todo | todo | |
| 3.1.5.4.15 | RUNSPACEPOOL_HOST_CALL Message | todo | todo | |
| 3.1.5.4.16 | RUNSPACEPOOL_HOST_RESPONSE Message | todo | todo | |
| 3.1.5.4.17 | PIPELINE_INPUT Message | todo | todo | |
| 3.1.5.4.18 | END_OF_PIPELINE_INPUT Message | todo | todo | |
| 3.1.5.4.19 | PIPELINE_OUTPUT Message | todo | todo | |
| 3.1.5.4.20 | ERROR_RECORD Message | todo | todo | |
| 3.1.5.4.21 | PIPELINE_STATE Message | todo | todo | |
| 3.1.5.4.22 | DEBUG_RECORD Message | todo | todo | |
| 3.1.5.4.23 | VERBOSE_RECORD Message | todo | todo | |
| 3.1.5.4.24 | WARNING_RECORD Message | todo | todo | |
| 3.1.5.4.25 | PROGRESS_RECORD Message | todo | todo | |
| 3.1.5.4.26 | INFORMATION_RECORD Message | todo | todo | |
| 3.1.5.4.27 | PIPELINE_HOST_CALL Message | todo | todo | |
| 3.1.5.4.28 | PIPELINE_HOST_RESPONSE Message | todo | todo | |
| 3.1.5.4.29 | CONNECT_RUNSPACEPOOL Message | todo | todo | |
| 3.1.5.4.30 | RUNSPACEPOOL_INIT_DATA Message | todo | todo | |
| 3.1.5.4.31 | RESET_RUNSPACE_STATE Message | todo | todo | |
| 3.1.6 | Timer Events | todo | todo | |
| 3.1.7 | Other Local Events | todo | todo | |

## Server Protocol Details (3.2)

**Out of scope: client-side only implementation.** These rows are `n/a` by design, not deferred.

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 3.2 | Server Details | n/a | n/a | |
| 3.2.1 | Abstract Data Model | n/a | n/a | |
| 3.2.1.1 | Global Data | n/a | n/a | |
| 3.2.1.1.1 | WSMV ShellID to RunspacePool Table | n/a | n/a | |
| 3.2.1.1.2 | WSMV CommandId to Pipeline Table | n/a | n/a | |
| 3.2.1.2 | RunspacePool Data | n/a | n/a | |
| 3.2.1.2.1 | GUID | n/a | n/a | |
| 3.2.1.2.2 | RunspacePool State | n/a | n/a | |
| 3.2.1.2.3 | Defragmentation Data | n/a | n/a | |
| 3.2.1.2.4 | Queue of Outgoing Messages | n/a | n/a | |
| 3.2.1.2.5 | HostInfo | n/a | n/a | |
| 3.2.1.2.6 | Host Calls CI Table | n/a | n/a | |
| 3.2.1.2.7 | Session Key | n/a | n/a | |
| 3.2.1.2.8 | Public Key | n/a | n/a | |
| 3.2.1.2.9 | Minimum and Maximum Number of Runspaces in the Pool | n/a | n/a | |
| 3.2.1.2.10 | Runspace Table | n/a | n/a | |
| 3.2.1.2.11 | Pending Pipelines Queue | n/a | n/a | |
| 3.2.1.3 | Pipeline Data | n/a | n/a | |
| 3.2.1.3.1 | GUID | n/a | n/a | |
| 3.2.1.3.2 | Pipeline State | n/a | n/a | |
| 3.2.1.3.3 | Defragmentation Data | n/a | n/a | |
| 3.2.1.3.4 | Queue of Outgoing Messages | n/a | n/a | |
| 3.2.1.3.5 | HostInfo | n/a | n/a | |
| 3.2.1.3.6 | Host Calls CI Table | n/a | n/a | |
| 3.2.1.4 | Runspace Data | n/a | n/a | |
| 3.2.1.4.1 | Runspace State | n/a | n/a | |
| 3.2.1.4.2 | Currently Running Pipeline | n/a | n/a | |
| 3.2.2 | Timers | n/a | n/a | |
| 3.2.3 | Initialization | n/a | n/a | |
| 3.2.4 | Higher-Layer Triggered Events | n/a | n/a | |
| 3.2.5 | Message Processing Events and Sequencing Rules | n/a | n/a | |
| 3.2.5.1 | General Rules | n/a | n/a | |
| 3.2.5.1.1 | Rules for Sending Data | n/a | n/a | |
| 3.2.5.1.2 | Rules for Receiving Data | n/a | n/a | |
| 3.2.5.2 | Sequencing Rules | n/a | n/a | |
| 3.2.5.3 | Rules for Processing WS-Man Messages | n/a | n/a | |
| 3.2.5.3.1 | Rules for the wxf:Create Message | n/a | n/a | |
