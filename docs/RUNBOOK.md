# Distributed Document System — Runbook

This comprehensive guide provides instructions for building, running, and using the Distributed Document System. The system consists of three main components: Name Server (NM), Storage Server (SS), and Client.

**System Requirements:**
- OS: Linux
- Shell: bash  
- Language: C (gcc, make)
- Network: TCP sockets

---

## Table of Contents
1. [Building the System](#1-building-the-system)
2. [Starting the System](#2-starting-the-system)
3. [Using the Client](#3-using-the-client)
4. [Command Reference](#4-command-reference)
5. [Testing](#5-testing)
6. [Troubleshooting](#6-troubleshooting)

---

## 1) Building the System

### Build from Source

Navigate to the project root directory and run:

```bash
make
```

This compiles all components and creates the following binaries in `bin/`:

| Binary | Description |
|--------|-------------|
| `bin/nm` | Name Server - Central coordinator |
| `bin/ss` | Storage Server - File storage and operations |
| `bin/client` | Interactive Client - User interface |
| `bin/test_driver` | Test Harness - Automated testing (optional) |

### Clean Build

To rebuild from scratch:

```bash
make clean
make
```

### Verify Build

Check that all binaries were created:

```bash
ls -lh bin/
```

---

## 2) Starting the System

The system requires **three terminal sessions** running simultaneously. Start them in order:

### Step 1: Start the Name Server (Terminal 1)

The Name Server acts as the central coordinator and **must be started first**.

```bash
bin/nm <port> <data_directory>
```

**Example:**
```bash
bin/nm 5000 nm_data
```

**Parameters:**
- `<port>` - Port number for NM to listen on (e.g., 5000)
- `<data_directory>` - Directory where NM persists metadata (e.g., nm_data)

**Expected Behavior:**
- No terminal output (runs in foreground)
- Check `nm.log` for: `[YYYY-MM-DD HH:MM:SS] INFO NM listening on port 5000`

**Setup:**
```bash
mkdir -p nm_data  # Create directory if it doesn't exist
```

---

### Step 2: Start the Storage Server (Terminal 2)

The Storage Server handles file storage and registers with the Name Server.

```bash
bin/ss <nm_host> <nm_port> <ss_port> <storage_directory>
```

**Example:**
```bash
bin/ss 127.0.0.1 5000 6000 ss_data
```

**Parameters:**
- `<nm_host>` - Name Server hostname/IP (e.g., 127.0.0.1)
- `<nm_port>` - Name Server port (must match NM's listening port)
- `<ss_port>` - Port for SS to listen on for client connections (e.g., 6000)
- `<storage_directory>` - Directory where files are stored (e.g., ss_data)

**Expected Behavior:**
- Check `ss.log` for:
  - `Registered to NM 127.0.0.1:5000 for port 6000`
  - `SS listening on 6000`

**Setup:**
```bash
mkdir -p ss_data/files ss_data/prev  # Create storage structure
```

**Storage Structure:**
```
ss_data/
├── files/      # Actual file content
└── prev/       # Undo backups (one-step)
```

**Note:** SS automatically retries registration with NM up to 30 times (1 second intervals) if connection fails.

---

### Step 3: Start the Client (Terminal 3)

The Client provides an interactive interface for users.

```bash
bin/client <nm_host> <nm_port>
```

**Example:**
```bash
bin/client 127.0.0.1 5000
```

**Parameters:**
- `<nm_host>` - Name Server hostname/IP
- `<nm_port>` - Name Server port

**Initial Interaction:**
```
Enter username: user1
Connected to NM. Enter commands (VIEW, READ, CREATE, WRITE, UNDO, INFO, DELETE, STREAM, LIST, ADDACCESS, REMACCESS, EXEC, QUIT).
> 
```

You are now ready to execute commands!

---

## 3) Using the Client

### Command Format

Commands are entered at the `>` prompt:

```
> COMMAND [flags] [arguments]
```

### Exiting

```
> QUIT
```

### Auto-Reconnection

If connection to NM is lost, the client automatically attempts to reconnect.

---

## 4) Command Reference

### 4.1 VIEW - List Files

View files you have access to:

```
> VIEW
```

**Output:** List of filenames
```
file1.txt
file2.txt
```

**Flags:**
- `-a` - Show all files in system (not just your accessible files)
- `-l` - Show detailed information (words, chars, owner, last access)
- `-al` - Combine both flags

**Examples:**

```
> VIEW -a
```
Shows all files regardless of access.

```
> VIEW -l
```
Shows detailed table:
```
---------------------------------------------------------
| Filename   | Words | Chars | Last Access        | Owner |
---------------------------------------------------------
| mouse.txt  |    12 |    60 | 2025-10-10 14:32   | user1 |
---------------------------------------------------------
```

```
> VIEW -al
```
Shows all files with details.

---

### 4.2 CREATE - Create New File

Create an empty file:

```
> CREATE <filename>
```

**Example:**
```
> CREATE document.txt
```

**Output:**
```
File Created Successfully!
```

**Notes:**
- Creates empty file on Storage Server
- You become the owner (full read/write access)
- File appears in subsequent VIEW commands
- Error if file already exists

---

### 4.3 READ - Display File Content

Read and display complete file content:

```
> READ <filename>
```

**Example:**
```
> READ document.txt
```

**Output:**
```
This is the content of the file.
```

**Requirements:**
- Must have read access to the file
- Displays entire file content

---

### 4.4 WRITE - Edit File Content

Edit a specific sentence in a file:

```
> WRITE <filename> <sentence_index>
```

**Interactive Mode:**
```
> WRITE document.txt 0
Enter edits as '<word_index> <text>' per line. End with ETIRW.
write> 1 Hello world.
write> ETIRW
Write successful
```

**Sentence Indexing:**
- Sentences are 0-indexed
- Sentence index 0 = first sentence
- Sentence index 1 = second sentence, etc.
- To append a new sentence, use index = current sentence count

**Word Editing:**
```
write> <word_index> <text>
```
- `<word_index>` - Position to insert text (1-indexed)
- `<text>` - Words to insert

**Multiple Edits:**
You can perform multiple edits before finishing:
```
write> 1 First
write> 2 Second
write> 3 Third
write> ETIRW
```

**Terminator:**
- Type `ETIRW` (WRITE backwards) to finish and commit changes

**Important Notes:**
- **Sentence Locking:** The sentence is locked for other users during your edit
- **Delimiters:** Period (.), exclamation (!), and question (?) mark create sentence boundaries
  - Example: `This is one. This is two!` creates 2 sentences
  - Even in middle of words: `e.g.` creates 2 sentences
- **Concurrent Edits:** Multiple users can edit different sentences simultaneously
- **Error Handling:**
  - Out of range sentence index returns error
  - Attempting to edit locked sentence returns error

**Example Session:**
```
> CREATE test.txt
File Created Successfully!

> WRITE test.txt 0
write> 1 First sentence.
write> ETIRW
Write successful

> WRITE test.txt 1
write> 1 Second sentence.
write> ETIRW
Write successful

> READ test.txt
First sentence. Second sentence.

> WRITE test.txt 0
write> 2 inserted
write> ETIRW
Write successful

> READ test.txt
First inserted sentence. Second sentence.
```

---

### 4.5 DELETE - Remove File

Delete a file (owner only):

```
> DELETE <filename>
```

**Example:**
```
> DELETE oldfile.txt
```

**Output:**
```
File deleted successfully!
```

**Requirements:**
- Only file owner can delete
- File must not be locked (no active WRITE operation)
- Removes file from both NM metadata and SS storage

**Errors:**
- Non-owner attempting delete: `ERR 4 Owner only`
- File is locked: `ERR 7 Locked`

---

### 4.6 UNDO - Revert Last Change

Undo the most recent modification to a file:

```
> UNDO <filename>
```

**Example:**
```
> UNDO document.txt
```

**Output:**
```
Undo Successful!
```

**Important Notes:**
- **One-step undo only:** Only the most recent change can be undone
- **File-specific, not user-specific:** Any user with write access can undo anyone's changes
- **Maintained by SS:** Undo history is stored in `ss_data/prev/`
- **No undo available:** If file hasn't been modified, returns error

**Example:**
```
> WRITE test.txt 0
write> 1 Original content.
write> ETIRW

> WRITE test.txt 0
write> 1 Modified content.
write> ETIRW

> UNDO test.txt
Undo Successful!

> READ test.txt
Original content.

> UNDO test.txt
ERR 6 No undo
```

---

### 4.7 INFO - Get File Metadata

Display detailed file information:

```
> INFO <filename>
```

**Example:**
```
> INFO document.txt
```

**Output:**
```
File: document.txt
Owner: user1
Created: 2025-11-07 14:21:00
Last Modified: 2025-11-07 14:32:02
Size: 52 bytes
Last Accessed: 2025-11-07 14:33:11 by user1
Access:
  user1 (RW)
  user2 (R)
```

**Information Displayed:**
- Filename
- Owner
- Created timestamp
- Last modified timestamp
- File size in bytes
- Last accessed time and user
- Access control list (ACL) with permissions

---

### 4.8 STREAM - Word-by-Word Streaming

Stream file content word by word with delay:

```
> STREAM <filename>
```

**Example:**
```
> STREAM document.txt
```

**Output:**
```
Hello
world
this
is
streaming
```

**Behavior:**
- Each word appears on a new line
- 0.1 second (100ms) delay between words
- Client connects directly to Storage Server
- Requires read access

**Error:**
If SS disconnects during streaming:
```
ERR 9 Storage server unavailable during streaming.
```

---

### 4.9 LIST - Show All Users

List all registered users:

```
> LIST
```

**Example:**
```
> LIST
```

**Output:**
```
user1
user2
alice
bob
```

**Notes:**
- Shows all usernames that have connected to the system
- Maintained by Name Server

---

### 4.10 ADDACCESS - Grant File Access

Grant read or write access to another user:

```
> ADDACCESS -R <filename> <username>   # Read access
> ADDACCESS -W <filename> <username>   # Write access (includes read)
```

**Examples:**

Grant read access:
```
> ADDACCESS -R private.txt user2
Access granted successfully!
```

Grant write access:
```
> ADDACCESS -W private.txt user2
Access granted successfully!
```

**Requirements:**
- Only file owner can grant access
- `-R` flag: Read-only access
- `-W` flag: Write access (automatically includes read)
- Owner always has full read/write access

**Effect:**
- User appears in file's ACL (shown in INFO command)
- User can now perform operations based on permission level

---

### 4.11 REMACCESS - Revoke File Access

Remove all access from a user:

```
> REMACCESS <filename> <username>
```

**Example:**
```
> REMACCESS private.txt user2
Access removed successfully!
```

**Requirements:**
- Only file owner can remove access
- Cannot remove owner's own access

**Error:**
```
> REMACCESS myfile.txt user1
ERR 4 Owner cannot remove their own access.
```

---

### 4.12 EXEC - Execute File as Shell Commands

Execute file content as bash commands on the Name Server:

```
> EXEC <filename>
```

**How EXEC Works:**
1. Reads file content from Storage Server
2. Splits content by NEWLINES (each line is one command)
3. Each non-empty line becomes a separate bash command
4. Commands execute sequentially on the Name Server
5. Output is captured and returned to client

**Example:**

File content (`diagnostics.txt`):
```
echo Running diagnostics
pwd
ls -lh | head -n 5
echo Done
```

This creates **4 separate commands**:
```bash
echo Running diagnostics
pwd
ls -lh | head -n 5
echo Done
```

Command:
```
> EXEC diagnostics.txt
```

**Output:**
```
Running diagnostics
/root/Distributed_File_System/
total 48K
-rw-r--r-- 1 root root 1.5K Nov  7 Makefile
-rw-r--r-- 1 root root  15K Nov  7 README.md
drwxr-xr-x 2 root root 4.0K Nov  7 bin
Done
```

**Creating Executable Files:**

Method 1: Put one command per line in a single WRITE:
```
> CREATE script.txt
> WRITE script.txt 0
write> 4 date
write> whoami
write> hostname
write> uptime
write> ETIRW
```

Method 2: Add lines across multiple WRITEs (newline-separated):
```
> CREATE script.txt
> WRITE script.txt 0
write> 1 echo Starting check
write> ETIRW
> WRITE script.txt 1
write> 1 uptime
write> ETIRW
> WRITE script.txt 2
write> 1 echo Done
write> ETIRW
```

**Important Rules:**

1. **Newlines Create Commands:**
   - One command per line
   - Empty lines are ignored

2. **Pipes and Redirections Work:**
   ```
   ls -la | grep txt
   df -h | head -n 10
   find . -name "*.c" | wc -l
   ```

3. **Multi-word Commands:**
   Keep entire command on one line:
   ```
   echo This is a long message
   ```

4. **Avoid unintended newlines inside commands:**
   - Commands must not span multiple lines
   - Use `;` to chain multiple operations on the same line

5. **Command Chaining:**
   Use `;` on the same line for multiple operations:
   ```
   cd /tmp; ls; pwd
   ```

**Sample Executable Files:**

**System Info:**
```
> WRITE sysinfo.txt 0
write> 5 echo System Information
write> hostname
write> uname -r
write> uptime
write> free -h | grep Mem
write> ETIRW
```

**File Counter:**
```
> WRITE count.txt 0
write> 3 echo Counting files
write> find . -type f | wc -l
write> echo files found
write> ETIRW
```

**Network Check:**
```
> WRITE netcheck.txt 0
write> 3 echo Network Status
write> hostname -I
write> netstat -tuln | grep LISTEN | head -n 5
write> ETIRW
```

**Date and Time:**
```
> WRITE datetime.txt 0
write> 4 echo Current date and time:
write> date
write> echo Uptime:
write> uptime
write> ETIRW
```

**Important Notes:**
- **Execution location:** Commands run on the **Name Server**, not client machine
- **Access requirement:** Requires **read access** to the file
- **Output capture:** All stdout and stderr are captured and returned
- **Error handling:** If a command fails, subsequent commands still execute
- **Working directory:** Commands execute in the NM's working directory

**Security Warning:** 
EXEC runs arbitrary shell commands on the Name Server with server privileges. Be cautious with:
- File operations that modify system files
- Commands that could consume resources
- Network operations
- Destructive commands

**Error Messages:**

```
ERR 1 No such file        # File doesn't exist
ERR 4 Forbidden           # No read access
ERR 9 Storage unavailable # SS disconnected
ERR 5 Exec failed         # Execution error
```

---

### 4.13 STATUS - Storage Server Health

Show health of registered Storage Servers as seen by the Name Server:

```
> STATUS
```

**Output:**
```
SS 1 127.0.0.1:6000 UP fails=0 last_ok=14:21:03 last_ck=14:21:03
SS 2 127.0.0.1:6001 DOWN fails=3 last_ok=14:18:50 last_ck=14:21:01
.
```

Notes:
- NM probes SS instances periodically and prefers healthy replicas
- Use this to diagnose routing/availability issues

---

## 5) Testing

### Manual Testing

Follow the command examples in Section 4 to manually test each feature.

### Automated Testing

The project includes an optional test harness:

```bash
# Build first
make

# Run tests (uses ports 5800 and 6800 by default)
tests/run_tests.sh
```

**Custom Ports:**
```bash
NM_PORT=5900 SS_PORT=6900 tests/run_tests.sh
```

**What it tests:**
- Basic file operations (CREATE, READ, WRITE, DELETE)
- Access control
- Multiple client scenarios

---

## 6) Troubleshooting

### Connection Refused

**Problem:** Client cannot connect to Name Server

**Solutions:**
1. Verify NM is running: `ps aux | grep bin/nm`
2. Check NM port in `nm.log`
3. Ensure client uses correct host:port
4. Check firewall settings

---

### Storage Server Not Registering

**Problem:** SS log shows repeated registration failures

**Solutions:**
1. Verify NM is running first
2. Check NM host and port in SS command
3. Wait for automatic retry (up to 30 attempts)
4. Check `nm.log` for connection attempts
5. Verify no firewall blocking ports

---

### File Not Found in VIEW

**Problem:** Created file doesn't appear in VIEW

**Solutions:**
1. Check if CREATE returned success message
2. Verify SS is running and registered
3. Check `ss.log` for file creation
4. Try VIEW -a to see all files
5. Verify file ownership and access

---

### Write Operation Fails

**Problem:** Cannot write to file

**Possible Causes:**

1. **No write access:**
   ```
   ERR 4 Forbidden
   ```
   Solution: File owner must grant you write access via ADDACCESS -W

2. **Sentence locked:**
   ```
   ERR 7 Sentence locked
   ```
   Solution: Another user is editing this sentence. Wait and retry.

3. **Sentence index out of range:**
   ```
   ERR 8 Sentence index out of range.
   ```
   Solution: Use valid sentence index (0 to sentence_count)

---

### Undo Not Working

**Problem:** UNDO returns "No undo" error

**Cause:** File hasn't been modified since creation or last undo

**Explanation:** 
- Undo only maintains one previous version
- Backup is created on first WRITE
- Subsequent WRITEs overwrite the backup
- UNDO restores backup and removes it

---

### Streaming Stops Abruptly

**Problem:** STREAM command stops mid-file

**Cause:** Storage Server disconnected during streaming

**Error Message:**
```
ERR 9 Storage server unavailable during streaming.
```

**Solutions:**
1. Check if SS is running
2. Restart SS if needed
3. Retry STREAM command

---

### Reset Environment

To completely reset the system:

```bash
# Stop all components (Ctrl+C in each terminal)

# Clean data
rm -rf nm_data ss_data *.log

# Recreate directories
mkdir -p nm_data ss_data/files ss_data/prev

# Restart NM and SS
```

---

## 7) System Architecture

### Component Overview

```
┌─────────┐         ┌──────────────┐         ┌─────────────┐
│ Client  │ ◄─────► │ Name Server  │ ◄─────► │  Storage    │
│         │         │     (NM)     │         │  Server(SS) │
└─────────┘         └──────────────┘         └─────────────┘
```

### Name Server (NM)

**Responsibilities:**
- Central coordinator
- Maintains file metadata (owner, ACL, statistics)
- Routes requests to appropriate Storage Server
- Handles user registration
- Executes EXEC commands
- Persists state to `nm_data/index.tsv`

**Port Usage:**
- Listens on configured port (e.g., 5000)
- Accepts connections from both Clients and Storage Servers

---

### Storage Server (SS)

**Responsibilities:**
- Physical file storage
- File operations (CREATE, READ, WRITE, DELETE)
- Sentence-level locking for concurrent edits
- One-step UNDO mechanism
- Word-by-word STREAMING

**Port Usage:**
- Connects to NM for registration
- Listens on configured port (e.g., 6000) for client requests

**Storage:**
- Files: `ss_data/files/<filename>`
- Undo backups: `ss_data/prev/<filename>.prev`

---

### Client

**Responsibilities:**
- User interface (interactive CLI)
- Command parsing and execution
- Connection management (auto-reconnect)

**Communication:**
- Connects to NM for most operations
- Direct connection to SS for READ, WRITE, STREAM

---

### File Structure

**Sentence Definition:**
- Sequence of words ending with `.`, `!`, or `?`
- Example: `Hello world. How are you?` = 2 sentences

**Word Definition:**
- Sequence of ASCII characters without spaces
- Words within sentence separated by spaces

**Concurrent Access:**
- Multiple users can read simultaneously
- Multiple users can write to different sentences simultaneously
- Sentence locked during edit (one user at a time per sentence)

---

### Logging

**NM Log (`nm.log`):**
- All client requests with username, IP, port
- File operations (CREATE, DELETE, access grants)
- Storage Server registrations
- Timestamps on all entries

**SS Log (`ss.log`):**
- Registration attempts and success
- File operations (CREATE, READ, WRITE, DELETE)
- Sentence locks
- Timestamps on all entries

---

### Data Persistence

**Name Server:**
- File metadata saved to `nm_data/index.tsv`
- Includes: filename, owner, SS location, ACL, statistics
- Loaded on NM startup
- Saved after each metadata change

**Storage Server:**
- File content in `ss_data/files/`
- Undo backups in `ss_data/prev/`
- No index file (NM maintains metadata)

---

## Quick Reference Card

### Startup Commands
```bash
# Terminal 1 - Name Server
bin/nm 5000 nm_data

# Terminal 2 - Storage Server
bin/ss 127.0.0.1 5000 6000 ss_data

# Terminal 3 - Client
bin/client 127.0.0.1 5000
```

### Common Client Commands
```bash
VIEW              # List my files
VIEW -al          # List all files with details
CREATE file.txt   # Create file
READ file.txt     # Read file
WRITE file.txt 0  # Edit first sentence
DELETE file.txt   # Delete file
INFO file.txt     # Show metadata
LIST              # Show users
QUIT              # Exit
```

### Access Control
```bash
ADDACCESS -R file.txt user2   # Grant read
ADDACCESS -W file.txt user2   # Grant write
REMACCESS file.txt user2      # Revoke access
```

---

**For additional support, check the logs (`nm.log`, `ss.log`) for detailed operation traces.**
