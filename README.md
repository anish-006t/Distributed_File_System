

# Distributed File System

## 📌 Overview

A distributed document storage system with fine-grained access control, concurrent editing, and multi-computer network support.

This project demonstrates core distributed systems concepts including client-server communication, synchronization, and scalable multi-node architecture.
## Quick Start

### Single Computer Setup

```bash
# Build
make

# Terminal 1: Name Server
./bin/nm 5000 nm_data

# Terminal 2: Storage Server
./bin/ss 127.0.0.1 5000 6000 ss_data

# Terminal 3: Client
./bin/client 127.0.0.1 5000
```

### Multi-Computer Network Setup

```bash
# Computer A (192.168.1.100): Name Server
./bin/nm 5000 nm_data

# Computer B (192.168.1.101): Storage Server
./bin/ss 192.168.1.100 5000 6000 ss_data

# Computer C (192.168.1.102): Client
./bin/client 192.168.1.100 5000
```

## Features

✅ **Distributed Architecture** – Supports multiple storage nodes and concurrent clients  
✅ **Multi-Computer Deployment** – Runs seamlessly across machines in a subnet  
✅ **Fine-Grained Access Control** – Owner-based permissions with ACL enforcement  
✅ **Concurrent Editing** – Sentence-level locking for safe multi-user edits  
✅ **Advanced Operations** – Includes streaming, undo, and remote command execution  

## Documentation

| Document | Purpose |
|----------|---------|
| **[RUNBOOK.md](docs/RUNBOOK.md)** | Complete operational guide with all commands |
| **[SYSTEM_FEATURES.md](SYSTEM_FEATURES.md)** | Comprehensive feature guide, limitations, roadmap |
| **[NETWORK_DEPLOYMENT.md](NETWORK_DEPLOYMENT.md)** | Multi-computer setup guide |


## Core Commands

```bash
CREATE <filename>              # Create file
READ <filename>                # Read content
WRITE <filename> <sent_idx>    # Edit sentence
DELETE <filename>              # Delete file
INFO <filename>                # Show metadata
VIEW [-a] [-l]                 # List files
ADDACCESS -R|-W <file> <user>  # Grant access
REMACCESS <file> <user>        # Revoke access
UNDO <filename>                # Undo last change
STREAM <filename>              # Stream word-by-word
EXEC <filename>                # Execute shell commands
LIST                           # Show users
```

## System Architecture

The Name Server acts as a central coordinator, routing client requests to appropriate storage servers while maintaining metadata.

```
┌─────────┐         ┌──────────────┐         ┌─────────────┐
│ Client  │ ◄─────► │ Name Server  │ ◄─────► │  Storage    │
│         │         │     (NM)     │         │  Server(SS) │
└─────────┘         └──────────────┘         └─────────────┘
```

- **Name Server (NM):** Central coordinator, metadata management
- **Storage Server (SS):** Distributed file storage, operations
- **Client:** Interactive user interface

## Network Configuration

✅ **Already Supports Multi-Computer Deployment**

The system uses `INADDR_ANY` (0.0.0.0) binding, making it accessible from any computer on the same subnet.

**Example Topology:**
```
Subnet: 192.168.1.0/24

Computer A (.100): Name Server
Computer B (.101): Storage Server 1
Computer C (.102): Storage Server 2
Computer D (.103): Client 1
Computer E (.104): Client 2
```

**See:** `NETWORK_DEPLOYMENT.md` for complete setup guide

**See:** `SYSTEM_FEATURES.md` for complete limitations and future improvements

## 🛠️ Tech Stack

- C (Core implementation)
- Socket Programming (Networking)
- Multi-threading (Concurrency)
- Distributed Systems Concepts

## Project Structure

```
bin/              # Compiled binaries
src/              # Source code
  client/         # Client implementation
  nm/             # Name Server implementation
  ss/             # Storage Server implementation
  common/         # Shared utilities
tests/            # Test suites
docs/             # Documentation
```

See individual documentation files for technical details:
- Architecture: `SYSTEM_FEATURES.md`
- Operations: `RUNBOOK.md`
- Network Setup: `NETWORK_DEPLOYMENT.md`


