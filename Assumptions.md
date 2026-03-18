# Assumptions

This document lists the assumptions used while designing and implementing the project. They are derived from the project specification and are intended to remove ambiguity for behaviours not explicitly defined in the assignment.

## General

- The Name Server (NM) is a single authoritative instance. NM failure is out-of-scope: if NM goes down the system is considered down and must be restarted (per spec).
- Usernames are unique across the system. A client provides a single username at startup which is used for all subsequent operations.
- The creator of a file is its owner. The owner always has both read and write access and ownership does not change unless the file is deleted and re-created.
- All hosts (clients, NM, SS) run in a relatively well-synchronized environment (e.g., NTP) so timestamps used for metadata are comparable. Perfect synchronization is not required; only ordering semantics are assumed.

## File model and content

- Files store text data only. Files are sequences of sentences; each sentence is a sequence of words.
- A sentence delimiter is any of: period (.), exclamation mark (!), or question mark (?). Every occurrence of these characters is considered a sentence delimiter (even inside abbreviations like "e.g."). Ellipses or consecutive punctuation result in empty-or-short sentences as they are parsed strictly by delimiter characters.
- Words are sequences of ASCII characters excluding spaces. The system assumes ASCII-only words for parsing and word-indexing. Non-ASCII characters may be treated as bytes or rejected depending on implementation; clients should use ASCII.
- File names must be a single token without path separators (no embedded "/" or nulls). File name character restrictions: printable ASCII excluding whitespace and slash. Filenames are unique across the system.
- There is no imposed logical limit on file size or number of files. Practically, the limit is the underlying SS storage capacity.

## Concurrency & locking

- Lock granularity is sentence-level. When a client issues a WRITE to a sentence, that sentence is exclusively locked for writing by others until the writer issues ETIRW (end write).
- Multiple readers can read the same sentence concurrently. A WRITE will block until any current readers of that sentence finish (if the implementation uses read/write locks), and readers will be able to proceed unless a writer already holds the sentence lock.
- Only one writer may hold a sentence lock at a time. If two clients request to acquire a lock on the same sentence, the NM/SS will grant it to one and return a resource contention error to the other, or queue them depending on implementation; the chosen behaviour must be consistent and documented in logs.
- The WRITE operation may perform multiple in-session updates (multiple <word_index> <content> lines) and then a final ETIRW. The SS must ensure atomic visibility: other clients should observe either the previous sentence or the fully-updated sentence after ETIRW.
- The implementation may use temporary swap files + atomic rename to provide atomic commit semantics for a file or sentence updates.

## Undo semantics

- UNDO <filename> reverts the last committed change to that file (file-specific, not user-specific). The undo history is maintained by the Storage Server holding that file.
- Undo history is persistent across SS restarts, up to available disk and storage policies. No global undo across multiple files is required.
- If there is no prior change to undo, UNDO returns an appropriate error code.

## Access control

- Files have an owner and an ACL (access-control list) stored persistently on the SS and indexed in NM. The owner can add/remove access for usernames.
- ADDACCESS -R grants read-only access. ADDACCESS -W grants write access (implicitly read access too). REMACCESS removes all access for the user.
- If a user does not have read access, attempts to READ, STREAM, EXEC, or VIEW the file result in an unauthorized error.

## Storage servers (SS) and persistence

- Each SS persists files and metadata (ACLs, undo history, timestamps) to durable storage. After an SS restart, its data is expected to be available.
- NM stores metadata needed for lookups (file -> SS mapping, file metadata cache) but the source of truth for file contents and persistent undo history is the SS hosting the file.
- New SS can register with NM at runtime and provide list of files they host. NM may rebalance or choose SS on CREATE/DELETE requests based on a simple policy (round-robin or hash) — the exact policy is an implementation detail but must be deterministic and documented.

## Initialization and discovery

- The NM IP:port are assumed known and provided to clients and SSs at startup (hard-coded or configured). Clients and SSs register themselves with NM on initialization.

## Read / Write / Streaming / Execute flows

- READ: client asks NM for file's SS; NM returns SS IP and client port; client connects to SS to fetch content.
- STREAM: client connects to SS (address provided by NM) and receives words with a 0.1s delay between words. If SS goes down mid-stream, client receives an error and stream terminates with an appropriate message.
- WRITE: client requests NM -> NM directs/forwards or instructs SS; SS enforces sentence locks. Only after ETIRW are changes visible to other clients. Moreover sentences follow a 0-indexed system while, words follow 1-index system.
- EXEC: Per spec, the NM executes the file content as shell commands (on NM host) and returns stdout/stderr to the client. Execution runs in the NM process environment. Security: we assume a trusted testing environment; sandboxing and privilege restrictions are out-of-scope unless explicitly implemented.

## Logging and error handling

- NM and SS log every request, acknowledgement and response with timestamps, IP, port, username, operation and status. Logs are used for debugging and auditing.
- The system uses a common set of error codes for situations such as: FILE_NOT_FOUND, PERMISSION_DENIED, RESOURCE_LOCKED, INVALID_ARGUMENT, INTERNAL_ERROR, SERVER_UNAVAILABLE, STREAM_INTERRUPTED, etc. Error codes and human-readable messages should be consistent across components.

## Search & performance

- NM uses an efficient index (e.g., hashmap/trie) to map filenames to SS entries; expected lookup is better than O(N) in average case. NM also implements an optional small LRU cache for recent lookups/metadata.

## Tests, limits and minor behaviours

- LIST returns the set of users who have ever registered (logged in) with NM, not just currently connected users.
- VIEW -l and VIEW -al return word/character counts and timestamps. INFO <filename> returns comprehensive metadata (size, owner, ACL, timestamps) but may not include computed counts which are available via VIEW -l.
- Timestamps shown in listing/info use ISO-8601 format.
- Undo history size is not artificially limited by the spec; practical limits depend on SS storage.

## Security and out-of-scope items

- Authentication beyond username uniqueness is out-of-scope (no password or cryptographic auth by default). Transport encryption (TLS) is optional and out-of-scope unless specifically implemented.
- Name Server fault-tolerance (replication, leader election) is out-of-scope.

## Implementation-friendly assumptions

- The system treats files as text files; binary files are unsupported.
- Any behaviour not explicitly covered here is to be implemented in the simplest consistent way and documented in the repository README or implementation notes.

## DELETE command
Deleting of a file can only be done by the owner of the file. No other user(even those with access) can delete that file.
