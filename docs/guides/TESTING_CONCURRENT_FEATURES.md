# Testing Guide: New Concurrent Features

## Feature 1: ADDACCESS User Validation

### What Changed
The `ADDACCESS` command now validates that the target user exists before granting access.

### Test Steps

**Terminal 1 - Start NM:**
```bash
bin/nm 5000 nm_data
```

**Terminal 2 - Start SS:**
```bash
bin/ss 127.0.0.1 5000 6000 ss_data
```

**Terminal 3 - user1 (owner):**
```bash
bin/client 127.0.0.1 5000
```
Commands:
```
user1
CREATE test.txt
ADDACCESS -W test.txt ghost    # Should FAIL - user 'ghost' doesn't exist
```

Expected output:
```
ERR 5 Invalid user or user does not exist
```

**Terminal 4 - user2 (register):**
```bash
bin/client 127.0.0.1 5000
```
Commands:
```
user2
VIEW    # Just to register user2
```

**Terminal 3 - user1 again:**
```
ADDACCESS -W test.txt user2    # Should SUCCEED now
```

Expected output:
```
Access granted successfully!
```

---

## Feature 2: Concurrent Sentence Writes

### What Changed
Multiple users can now edit the SAME file simultaneously, as long as they're editing DIFFERENT sentences.

### Test Steps

**Setup (same terminals as above)**

**Terminal 3 - user1:**
```bash
bin/client 127.0.0.1 5000
```
Commands:
```
user1
CREATE concurrent.txt
WRITE concurrent.txt 0
1 First sentence here.
ETIRW
WRITE concurrent.txt 1
1 Second sentence here.
ETIRW
WRITE concurrent.txt 2
1 Third sentence here.
ETIRW
READ concurrent.txt
```

You should see:
```
First sentence here. Second sentence here. Third sentence here.
```

### Test Concurrent Edits to DIFFERENT Sentences (Should Work)

**Terminal 3 - user1 starts editing sentence 0:**
```
WRITE concurrent.txt 0
```
> DON'T enter the text yet! Just start the WRITE command.

**Terminal 4 - user2 starts editing sentence 1 (different!):**
```
user2
WRITE concurrent.txt 1
```
> This should SUCCEED immediately (no blocking)

Expected output in Terminal 4:
```
> Enter edits as '<word_index> <text>' per line. End with ETIRW.
write>
```

Now complete both edits:

**Terminal 3 (user1 on sentence 0):**
```
1 [EDITED BY USER1]
ETIRW
```

**Terminal 4 (user2 on sentence 1):**
```
1 [EDITED BY USER2]
ETIRW
```

Both should succeed! Read the file to verify:

**Terminal 3:**
```
READ concurrent.txt
```

Expected:
```
First [EDITED BY USER1] sentence here. Second [EDITED BY USER2] sentence here. Third sentence here.
```

---

### Test Concurrent Edits to SAME Sentence (Should Block)

**Terminal 3 - user1 starts editing sentence 0:**
```
WRITE concurrent.txt 0
```
> Again, DON'T enter text yet

**Terminal 4 - user2 tries to edit sentence 0 (SAME!):**
```
WRITE concurrent.txt 0
```

Expected output in Terminal 4:
```
ERR 7 Sentence locked by another user
```

✅ This is correct! user2 is blocked because user1 is already editing sentence 0.

Now complete user1's edit:

**Terminal 3:**
```
1 [USER1 FINISHED]
ETIRW
```

Now user2 can try again:

**Terminal 4:**
```
WRITE concurrent.txt 0
1 [NOW USER2 CAN EDIT]
ETIRW
```

This should succeed! ✅

---

## Summary of Changes

### 1. User Validation for ADDACCESS

**Files Modified:**
- `src/nm/nm_state.h` - Added `nm_user_exists()` function
- `src/nm/nm_state.c` - Implemented `nm_user_exists()` 
- `src/nm/main.c` - Added validation check before granting access

**Behavior:**
- Before: Could grant access to any username (even non-existent)
- After: Returns error if user doesn't exist in registry

---

### 2. Per-Sentence Locking

**Files Modified:**
- `src/ss/ss_state.h` - Changed from single `locked` flag to `SentenceLock` linked list
- `src/ss/ss_state.c` - Updated to manage lock list
- `src/ss/main.c` - Implemented per-sentence locking with helper functions

**Data Structure:**
```c
typedef struct SentenceLock {
    int sentence_idx;
    char user[128];
    struct SentenceLock *next;
} SentenceLock;
```

**Behavior:**
- Before: Only ONE user could edit a file at a time (file-level lock)
- After: Multiple users can edit DIFFERENT sentences simultaneously
- Same sentence: Still blocked (sentence-level lock)

**Thread Safety:**
- Uses mutex `G_LOCK_MUTEX` to protect lock list operations
- Lock list is checked/modified atomically

**Sentence Indices:**
- Indices remain sequential (0, 1, 2, 3...)
- After edit, sentences are stored in order
- Next WRITE operations use correct indices

---

## Implementation Details

### Lock Management Functions

```c
// Check if sentence is locked by someone else
static bool is_sentence_locked(FileRec *fr, int sentence_idx, const char *user);

// Acquire lock on a sentence
static bool lock_sentence(FileRec *fr, int sentence_idx, const char *user);

// Release lock on a sentence
static void unlock_sentence(FileRec *fr, int sentence_idx, const char *user);

// Check if any sentence in file is locked
static bool is_file_locked(FileRec *fr);
```

### WRITE Flow

1. User requests `WRITE filename sentence_idx`
2. SS checks if `sentence_idx` is locked by someone else
3. If locked → Return `ERR Sentence locked by another user`
4. If not locked → Acquire lock and proceed
5. User sends edits ending with `ETIRW`
6. SS applies edits, saves file
7. **SS releases lock** - Other users can now edit that sentence
8. Send success response

### DELETE Flow

1. User requests `DELETE filename`
2. SS checks if ANY sentence is locked (using `is_file_locked()`)
3. If any lock exists → Return `ERR Locked`
4. If no locks → Delete file

---

## Edge Cases Handled

1. **Same user, same sentence**: Allowed (user can re-enter edit mode)
2. **Different users, different sentences**: Allowed (concurrent editing)
3. **Different users, same sentence**: Blocked until first user finishes
4. **Client disconnects mid-write**: Lock is released (cleanup on connection close)
5. **Sentence index out of range**: Rejected before locking

---

## Testing Checklist

- [x] ADDACCESS rejects non-existent users
- [x] ADDACCESS accepts registered users
- [x] Two users can edit different sentences concurrently
- [x] Two users cannot edit same sentence concurrently
- [x] Locks are released after ETIRW
- [x] Sentence indices remain sequential after edits
- [x] File DELETE is blocked while any sentence is locked
- [x] Thread-safe lock management (mutex protection)
