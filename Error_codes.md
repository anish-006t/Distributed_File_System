# Error Codes Reference

This document enumerates all error codes defined in `src/common/errors.h`, describes their typical messages as implemented across the codebase, and explains the triggering conditions and rationale. It is generated from current usages observed in `src/nm/main.c` and `src/ss/main.c`.

## Legend
- Code: Enumeration constant from `enum ErrorCode`.
- Value: Numeric value (stable contract for protocol messages).
- Typical Messages: Representative strings passed to `proto_send_err`.
- Occurs When: Situations detected in code.
- Rationale: Why the error is surfaced / semantics.

---

## ERR_OK (0)
- Typical Messages: (Not sent via error path; represents success.)
- Occurs When: Internal success state; not emitted through `proto_send_err`.
- Rationale: Placeholder for explicit success; included for completeness.

## ERR_UNKNOWN (1)
- Typical Messages: Rare / currently not explicitly used; fallback could be "Unknown".
- Occurs When: Would be used for unspecified failures; presently most unknown cases map to specific codes instead. Seen once: `ERR_BAD_REQUEST` with message "Unknown" rather than ERR_UNKNOWN.
- Rationale: Reserved for future ambiguous failures.

## ERR_BAD_REQUEST (2)
- Typical Messages: "Empty", "Missing filename", "Usage: CREATE <file> <owner>", "Filename too long", "Usage: WRITE <file> <sentence_index>", "Missing username", "Unknown command", "Flag", "Usage: ADDACCESS -R|-W <file> <user>" etc.
- Occurs When:
  - Command token count (`tc`/`tc2`) insufficient.
  - Invalid command flag or malformed syntax.
  - Filename length exceeds allowed ( >255 ).
  - Unknown command strings at NM or SS level.
  - Missing required parameters (username, sentence index, file name).
- Rationale: Signals client-side input validation failure; client should correct request format or parameters.

## ERR_UNAUTHORIZED (3)
- Typical Messages: "Register first".
- Occurs When: A client attempts an operation before registering a username with NM.
- Rationale: Enforces initial user registration handshake prior to issuing privileged commands.

## ERR_FORBIDDEN (4)
- Typical Messages: "No access", "Owner only", "No write access", "Owner cannot remove their own access.".
- Occurs When:
  - User lacks read or write ACL for a file (READ/INFO/STREAM/EXEC/WRITE paths).
  - Attempt to perform owner-restricted operations (ADDACCESS, REMACCESS, DELETE) by non-owner.
  - Attempt by owner to remove their own access (explicitly blocked).
- Rationale: Access control enforcement distinct from authentication (registered) and existence (NOT_FOUND).

## ERR_NOT_FOUND (5)
- Typical Messages: "No file", "No such file", "Invalid user or user does not exist", "File missing on replicas", "User X never had this access".
- Occurs When:
  - File metadata absent in NM mapping / SS file registry.
  - Target username in ACL operations does not exist or was never granted access.
  - Replica set inconsistent (delete path) where file absent on one/more replicas.
  - SS reports missing file for operations like WRITE, READ, UNDO.
- Rationale: Distinguishes absence from permission errors; clients may choose to CREATE or verify identifiers.

## ERR_CONFLICT (6)
- Typical Messages: "File exists", "User already has this access", "No undo".
- Occurs When:
  - Creating a file that already exists (NM side pre-insertion check).
  - Adding access for a user who already possesses that level (duplicate grant).
  - Undo operation invoked with empty/no available history.
  - SS propagated conflict for certain sentence lock or state mismatches (e.g., initial handshake returning a generic conflict). Also SS usage via NM propagation: sentence lock conflict mapping to ERR_LOCKED or ERR_CONFLICT depending path.
- Rationale: Represents state conflicts where the request cannot be fulfilled due to current resource state; client may need different action.

## ERR_LOCKED (7)
- Typical Messages: "Locked", "Sentence locked by another user", "Write in progress", "One or more replicas locked".
- Occurs When:
  - Attempting WRITE on a locked sentence (either at NM coordinating multi-SS or SS local lock state).
  - Attempting file-level operations (DELETE) while a write lock active on replicas.
  - Attempting operations during an in-progress write (SS or NM detects lock).
- Rationale: Concurrency control — informs client to retry later or release conflicting locks.

## ERR_RANGE (8)
- Typical Messages: "Sentence index out of range.", "Word index out of range".
- Occurs When:
  - Provided sentence index beyond current sentence count in WRITE acquisition or operations.
  - Provided word index beyond bounds during in-session WRITE updates.
  - NM mapping of SS write failure with explicit range errors (e.g., streaming intermediate responses with "ERR" prefix mapped to RANGE).
- Rationale: Invalid positional reference in document model; client must adjust indices using updated file state.

## ERR_INTERNAL (9)
- Typical Messages: "Read fail", "No storage server", "Failed to create file on required replicas", "SS error", "Exec failed", "Source connect failed", "Source read failed", "Rename fail", "Write temp fail", "Open temp fail", "No available servers", "Storage server unavailable during streaming.", "SS no response".
- Occurs When:
  - Resource acquisition failures (no active SS, no replicas, connection failures, file I/O issues, rename/move atomic commit failures).
  - Execution subsystem failures executing shell commands (NM side EXEC).
  - Inter-server communication anomalies (send/receive errors, unexpected responses or negative returns).
  - Low-level file operations (temporary swap file creation, sync reading, write flush) fail.
- Rationale: Indicates server-side failure independent of client request validity. Client may retry; persistent failures imply administrator intervention.

## ERR_CONSISTENCY (10)
- Typical Messages: "Replica delete mismatch".
- Occurs When:
  - Replica operations (DELETE) yield inconsistent outcomes (e.g., majority success but one mismatch reported).
- Rationale: Highlights distributed state divergence requiring remediation or reconciliation logic.

---

## Usage Patterns & Guidance
- `ERR_BAD_REQUEST` vs `ERR_FORBIDDEN`: BAD_REQUEST covers syntactic/structural input errors; FORBIDDEN covers validly structured requests lacking privilege.
- `ERR_NOT_FOUND` vs `ERR_CONFLICT`: NOT_FOUND when entity absent; CONFLICT when entity present but action invalid due to existing state.
- `ERR_LOCKED` vs `ERR_CONFLICT`: LOCKED is a concurrency-specific denial (resource locked); CONFLICT broader semantic state mismatch.
- `ERR_INTERNAL` should not leak sensitive internal details; current messages are operational but can be sanitized further if needed.
- `ERR_RANGE` encourages client to refresh sentence indices (possibly via REFRESH or re-READ) before retrying WRITE.
- `ERR_CONSISTENCY` signals distributed repair need; future work could trigger automatic reconciliation or mark replicas stale.

## Future Extension Suggestions
- Utilize `ERR_UNKNOWN` for truly unmapped failures where classification impossible.
- Add `ERR_TIMEOUT` if network operations adopt explicit timeout handling.
- Add `ERR_STREAM_INTERRUPTED` distinct from generic INTERNAL for mid-stream failures (currently INTERNAL message used).

## Source Files Referenced
- Error enum: `src/common/errors.h`
- NM usage contexts: `src/nm/main.c`
- SS usage contexts: `src/ss/main.c`

---
If you would like an auto-generated table version (e.g., CSV/TSV) or integration into protocol documentation, let me know.
