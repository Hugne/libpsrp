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
| 2.2.1 | PowerShell Remoting Protocol Message | todo | todo | |
| 2.2.4 | Packet Fragment | todo | todo | |

## Message Types (2.2.2)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.2 | Message Types | todo | todo | |
| 2.2.2.1 | SESSION_CAPABILITY Message | todo | todo | |
| 2.2.2.2 | INIT_RUNSPACEPOOL Message | todo | todo | |
| 2.2.2.3 | PUBLIC_KEY Message | todo | todo | |
| 2.2.2.4 | ENCRYPTED_SESSION_KEY Message | todo | todo | |
| 2.2.2.5 | PUBLIC_KEY_REQUEST Message | todo | todo | |
| 2.2.2.6 | SET_MAX_RUNSPACES Message | todo | todo | |
| 2.2.2.7 | SET_MIN_RUNSPACES Message | todo | todo | |
| 2.2.2.8 | RUNSPACE_AVAILABILITY Message | todo | todo | |
| 2.2.2.9 | RUNSPACEPOOL_STATE Message | todo | todo | |
| 2.2.2.10 | CREATE_PIPELINE Message | todo | todo | |
| 2.2.2.11 | GET_AVAILABLE_RUNSPACES Message | todo | todo | |
| 2.2.2.12 | USER_EVENT Message | todo | todo | |
| 2.2.2.13 | APPLICATION_PRIVATE_DATA Message | todo | todo | |
| 2.2.2.14 | GET_COMMAND_METADATA Message | todo | todo | |
| 2.2.2.15 | RUNSPACEPOOL_HOST_CALL Message | todo | todo | |
| 2.2.2.16 | RUNSPACEPOOL_HOST_RESPONSE Message | todo | todo | |
| 2.2.2.17 | PIPELINE_INPUT Message | todo | todo | |
| 2.2.2.18 | END_OF_PIPELINE_INPUT Message | todo | todo | |
| 2.2.2.19 | PIPELINE_OUTPUT Message | todo | todo | |
| 2.2.2.20 | ERROR_RECORD Message | todo | todo | |
| 2.2.2.21 | PIPELINE_STATE Message | todo | todo | |
| 2.2.2.22 | DEBUG_RECORD Message | todo | todo | |
| 2.2.2.23 | VERBOSE_RECORD Message | todo | todo | |
| 2.2.2.24 | WARNING_RECORD Message | todo | todo | |
| 2.2.2.25 | PROGRESS_RECORD Message | todo | todo | |
| 2.2.2.26 | INFORMATION_RECORD Message | todo | todo | |
| 2.2.2.27 | PIPELINE_HOST_CALL Message | todo | todo | |
| 2.2.2.28 | PIPELINE_HOST_RESPONSE Message | todo | todo | |
| 2.2.2.29 | CONNECT_RUNSPACEPOOL Message | todo | todo | |
| 2.2.2.30 | RUNSPACEPOOL_INIT_DATA Message | todo | todo | |
| 2.2.2.31 | RESET_RUNSPACE_STATE Message | todo | todo | |

## Other Object Types (2.2.3)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.3 | Other Object Types | todo | todo | |
| 2.2.3.1 | Coordinates | todo | todo | |
| 2.2.3.2 | Size | todo | todo | |
| 2.2.3.3 | Color | todo | todo | |
| 2.2.3.4 | RunspacePoolState | todo | todo | |
| 2.2.3.5 | PSInvocationState | todo | todo | |
| 2.2.3.6 | PSThreadOptions | todo | todo | |
| 2.2.3.7 | ApartmentState | todo | todo | |
| 2.2.3.8 | RemoteStreamOptions | todo | todo | |
| 2.2.3.9 | ErrorCategory | todo | todo | |
| 2.2.3.10 | TimeZone | todo | todo | |
| 2.2.3.10.1 | CurrentSystemTimeZone | todo | todo | |
| 2.2.3.10.2 | Hashtable from int to DaylightTime Using Default Comparer | todo | todo | |
| 2.2.3.10.3 | DaylightTime | todo | todo | |
| 2.2.3.11 | Pipeline | todo | todo | |
| 2.2.3.12 | Command | todo | todo | |
| 2.2.3.13 | Command Parameter | todo | todo | |
| 2.2.3.14 | HostInfo | todo | todo | |
| 2.2.3.15 | ErrorRecord | todo | todo | |
| 2.2.3.15.1 | InvocationInfo-specific Extended Properties | todo | todo | |
| 2.2.3.16 | InformationalRecord (DebugRecord, WarningRecord or VerboseRecord) | todo | todo | |
| 2.2.3.17 | Host Method Identifier | todo | todo | |
| 2.2.3.18 | Primitive Dictionary | todo | todo | |
| 2.2.3.19 | CommandType | todo | todo | |
| 2.2.3.20 | Wildcard | todo | todo | |
| 2.2.3.21 | CommandMetadataCount | todo | todo | |
| 2.2.3.22 | CommandMetadata | todo | todo | |
| 2.2.3.23 | ParameterMetadata | todo | todo | |
| 2.2.3.24 | ArgumentList | todo | todo | |
| 2.2.3.25 | PSCredential | todo | todo | |
| 2.2.3.26 | KeyInfo | todo | todo | |
| 2.2.3.27 | ControlKeyStates | todo | todo | |
| 2.2.3.28 | BufferCell | todo | todo | |
| 2.2.3.29 | BufferCellType | todo | todo | |
| 2.2.3.30 | CommandOrigin | todo | todo | |
| 2.2.3.31 | PipelineResultTypes | todo | todo | |

## Serialization (2.2.5)

| Section | Title | Impl | Tests | Notes |
|---|---|---|---|---|
| 2.2.5 | Serialization | todo | todo | |
| 2.2.5.1 | Serialization of Primitive Type Objects | todo | todo | |
| 2.2.5.1.1 | String | todo | todo | |
| 2.2.5.1.2 | Character | todo | todo | |
| 2.2.5.1.3 | Boolean | todo | todo | |
| 2.2.5.1.4 | Date/Time | todo | todo | |
| 2.2.5.1.5 | Duration | todo | todo | |
| 2.2.5.1.6 | Unsigned Byte | todo | todo | |
| 2.2.5.1.7 | Signed Byte | todo | todo | |
| 2.2.5.1.8 | Unsigned Short | todo | todo | |
| 2.2.5.1.9 | Signed Short | todo | todo | |
| 2.2.5.1.10 | Unsigned Int | todo | todo | |
| 2.2.5.1.11 | Signed Int | todo | todo | |
| 2.2.5.1.12 | Unsigned Long | todo | todo | |
| 2.2.5.1.13 | Signed Long | todo | todo | |
| 2.2.5.1.14 | Float | todo | todo | |
| 2.2.5.1.15 | Double | todo | todo | |
| 2.2.5.1.16 | Decimal | todo | todo | |
| 2.2.5.1.17 | Array of Bytes | todo | todo | |
| 2.2.5.1.18 | GUID | todo | todo | |
| 2.2.5.1.19 | URI | todo | todo | |
| 2.2.5.1.20 | Null Value | todo | todo | |
| 2.2.5.1.21 | Version | todo | todo | |
| 2.2.5.1.22 | XML Document | todo | todo | |
| 2.2.5.1.23 | ScriptBlock | todo | todo | |
| 2.2.5.1.24 | Secure String | todo | todo | |
| 2.2.5.1.25 | Progress Record | todo | todo | |
| 2.2.5.1.26 | Information Record | todo | todo | |
| 2.2.5.2 | Serialization of Complex Objects | todo | todo | |
| 2.2.5.2.1 | Referencing Earlier Objects | todo | todo | |
| 2.2.5.2.1.1 | RefId Attribute | todo | todo | |
| 2.2.5.2.1.2 | <Ref> Element | todo | todo | |
| 2.2.5.2.2 | <Obj> Element | todo | todo | |
| 2.2.5.2.3 | Type Names | todo | todo | |
| 2.2.5.2.4 | ToString | todo | todo | |
| 2.2.5.2.5 | Contents of Extended Primitive Objects | todo | todo | |
| 2.2.5.2.6 | Contents of Known Containers | todo | todo | |
| 2.2.5.2.6.1 | Stack | todo | todo | |
| 2.2.5.2.6.2 | Queue | todo | todo | |
| 2.2.5.2.6.3 | List | todo | todo | |
| 2.2.5.2.6.4 | Dictionaries | todo | todo | |
| 2.2.5.2.7 | Contents of Enums | todo | todo | |
| 2.2.5.2.8 | Adapted Properties | todo | todo | |
| 2.2.5.2.9 | Extended Properties | todo | todo | |
| 2.2.5.3 | Miscellaneous | todo | todo | |
| 2.2.5.3.1 | Property Name | todo | todo | |
| 2.2.5.3.2 | Encoding Strings | todo | todo | |
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
| 2.2.6 | Encoding Host Parameters in Host Method Calls | todo | todo | |
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
| 3.1 | Client Details | todo | todo | |
| 3.1.1 | Abstract Data Model | todo | todo | |
| 3.1.1.1 | Global Data | todo | todo | |
| 3.1.1.1.1 | WSMV ShellID to RunspacePool Table | todo | todo | |
| 3.1.1.1.2 | WSMV CommandId to Pipeline Table | todo | todo | |
| 3.1.1.1.3 | Public Key Pair | todo | todo | |
| 3.1.1.2 | RunspacePool Data | todo | todo | |
| 3.1.1.2.1 | GUID | todo | todo | |
| 3.1.1.2.2 | RunspacePool State | todo | todo | |
| 3.1.1.2.3 | Defragmentation Data | todo | todo | |
| 3.1.1.2.4 | WSMV Shell | todo | todo | |
| 3.1.1.2.5 | RunspacePool Information CI Table | todo | todo | |
| 3.1.1.2.6 | Pipeline Table | todo | todo | |
| 3.1.1.2.7 | Session Key | todo | todo | |
| 3.1.1.2.8 | SessionKeyTransferTimeoutms | todo | todo | |
| 3.1.1.3 | Pipeline Data | todo | todo | |
| 3.1.1.3.1 | GUID | todo | todo | |
| 3.1.1.3.2 | Pipeline State | todo | todo | |
| 3.1.1.3.3 | Defragmentation Data | todo | todo | |
| 3.1.1.3.4 | WSMV Command | todo | todo | |
| 3.1.2 | Timers | todo | todo | |
| 3.1.3 | Initialization | todo | todo | |
| 3.1.4 | Higher-Layer Triggered Events | todo | todo | |
| 3.1.4.1 | Creating a RunspacePool | todo | todo | |
| 3.1.4.2 | Closing a RunspacePool | todo | todo | |
| 3.1.4.3 | Executing a Pipeline | todo | todo | |
| 3.1.4.4 | Stopping a Pipeline | todo | todo | |
| 3.1.4.5 | Getting Command Metadata | todo | todo | |
| 3.1.4.6 | Setting the Minimum or Maximum Runspaces in a RunspacePool | todo | todo | |
| 3.1.4.7 | Getting the Number of Available Runspaces in a RunspacePool | todo | todo | |
| 3.1.4.8 | Initiating a Session Key Exchange | todo | todo | |
| 3.1.4.9 | Disconnecting from a RunspacePool | todo | todo | |
| 3.1.4.10 | Connecting to a RunspacePool | todo | todo | |
| 3.1.4.10.2 | Connecting to a RunspacePool from a Previous Client Session | todo | todo | |
| 3.1.4.10.3 | Connecting to a RunspacePool from a New Client Session | todo | todo | |
| 3.1.5 | Message Processing Events and Sequencing Rules | todo | todo | |
| 3.1.5.1 | General Rules | todo | todo | |
| 3.1.5.1.1 | Rules for Sending Data | todo | todo | |
| 3.1.5.1.2 | Rules for Receiving Data | todo | todo | |
| 3.1.5.2 | Sequencing Rules | todo | todo | |
| 3.1.5.3 | Rules for Processing WS-MAN Messages | todo | todo | |
| 3.1.5.3.1 | Rules for the wxf:Create Message | todo | todo | |
| 3.1.5.3.2 | Rules for the wxf:ResourceCreated Message | todo | todo | |
| 3.1.5.3.3 | Rules for the wxf:Command Message | todo | todo | |
| 3.1.5.3.4 | Rules for the wxf:CommandResponse Message | todo | todo | |
| 3.1.5.3.5 | Rules for the wxf:Send Message | todo | todo | |
| 3.1.5.3.6 | Rules for the wxf:SendResponse Message | todo | todo | |
| 3.1.5.3.7 | Rules for the wxf:Receive Message | todo | todo | |
| 3.1.5.3.8 | Rules for the wxf:ReceiveResponse Message | todo | todo | |
| 3.1.5.3.9 | Rules for the wxf:Signal Message | todo | todo | |
| 3.1.5.3.10 | Rules for the wxf:SignalResponse Message | todo | todo | |
| 3.1.5.3.11 | Rules for the wxf:Delete Message | todo | todo | |
| 3.1.5.3.12 | Rules for the wxf:DeleteResponse Message | todo | todo | |
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
