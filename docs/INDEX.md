# Documentation Index

This directory contains comprehensive documentation for the Network File System project.

## Quick Start
- [README.md](../README.md) - Project overview and getting started
- [RUNBOOK.md](RUNBOOK.md) - Operations and deployment guide

## User Guides
Located in `docs/guides/`:

- **[SYSTEM_FEATURES.md](guides/SYSTEM_FEATURES.md)** - Complete feature reference and usage
- **[QUICK_REFERENCE.md](guides/QUICK_REFERENCE.md)** - Quick command reference
- **[NETWORK_DEPLOYMENT.md](guides/NETWORK_DEPLOYMENT.md)** - Multi-computer deployment setup
- **[MULTIPLE_SS_GUIDE.md](guides/MULTIPLE_SS_GUIDE.md)** - Running multiple storage servers
- **[TESTING_CONCURRENT_FEATURES.md](guides/TESTING_CONCURRENT_FEATURES.md)** - Testing concurrent operations
- **[TEST_SUITE_README.md](guides/TEST_SUITE_README.md)** - Automated test suite documentation

## Critical Fixes & Implementation Details
Located in `docs/fixes/`:

### Bug Fixes
- **[DELIMITER_FIX.md](fixes/DELIMITER_FIX.md)** - Natural punctuation support in text
- **[CONCURRENCY_FIX.md](fixes/CONCURRENCY_FIX.md)** - Lost update prevention in concurrent writes
- **[UNDO_FLASHCARD_SYSTEM.md](fixes/UNDO_FLASHCARD_SYSTEM.md)** - Per-session undo implementation
- **[UNDO_FIX.md](fixes/UNDO_FIX.md)** - UNDO command fixes
- **[SENTENCE_DELIMITER_FIX.md](fixes/SENTENCE_DELIMITER_FIX.md)** - Sentence delimiter handling

### Implementation Summaries
- **[CRITICAL_FIXES.md](fixes/CRITICAL_FIXES.md)** - Overview of all critical bug fixes
- **[IMPLEMENTATION_SUMMARY.md](fixes/IMPLEMENTATION_SUMMARY.md)** - Technical implementation details
- **[FINAL_SUMMARY.md](fixes/FINAL_SUMMARY.md)** - Complete project summary

## Testing
- **[Test Suite](../tests/all_tests.sh)** - Automated test script (45 test cases)
- **[Test Documentation](guides/TEST_SUITE_README.md)** - Test suite usage and details

## Architecture Overview

### Core Components
1. **Naming Server (NM)** - Central coordination and file tracking
2. **Storage Servers (SS)** - Distributed file storage with sentence-level editing
3. **Client** - User interface for file operations

### Key Features
- Sentence-level file editing with concurrent access
- Optimistic concurrency control with re-read on commit
- Per-session undo with unlimited depth (flashcard system)
- Distributed storage with automatic failover
- Network streaming and asynchronous writes

### Concurrency Model
- Sentence-level locking using mutex protection
- Optimistic concurrency: re-read file at ETIRW commit time
- Lost update prevention through current-state validation
- Multiple users can edit different sentences simultaneously

### UNDO System
- Stack-based flashcard system
- One flashcard per WRITE session
- Unlimited undo depth
- Per-file undo tracking
- Preserves user and timestamp metadata

## Navigation by Topic

### Setting Up
1. Read [README.md](../README.md) for build instructions
2. Follow [RUNBOOK.md](RUNBOOK.md) for deployment
3. Use [NETWORK_DEPLOYMENT.md](guides/NETWORK_DEPLOYMENT.md) for multi-computer setup

### Using the System
1. Check [SYSTEM_FEATURES.md](guides/SYSTEM_FEATURES.md) for all commands
2. Use [QUICK_REFERENCE.md](guides/QUICK_REFERENCE.md) for command syntax
3. See [TESTING_CONCURRENT_FEATURES.md](guides/TESTING_CONCURRENT_FEATURES.md) for concurrent operations

### Understanding the Fixes
1. [CRITICAL_FIXES.md](fixes/CRITICAL_FIXES.md) - What was fixed and why
2. [DELIMITER_FIX.md](fixes/DELIMITER_FIX.md) - Natural text support
3. [CONCURRENCY_FIX.md](fixes/CONCURRENCY_FIX.md) - Concurrent write handling
4. [UNDO_FLASHCARD_SYSTEM.md](fixes/UNDO_FLASHCARD_SYSTEM.md) - UNDO implementation

### Testing
1. [TEST_SUITE_README.md](guides/TEST_SUITE_README.md) - Test suite overview
2. Run `tests/all_tests.sh` for automated testing
3. See [TESTING_CONCURRENT_FEATURES.md](guides/TESTING_CONCURRENT_FEATURES.md) for manual testing

## File Locations

```
Distributed_File_System/
├── README.md                    # Project overview
├── Makefile                     # Build system
├── docs/
│   ├── INDEX.md                 # This file
│   ├── RUNBOOK.md              # Operations guide
│   ├── fixes/                   # Bug fix documentation
│   │   ├── CRITICAL_FIXES.md
│   │   ├── DELIMITER_FIX.md
│   │   ├── CONCURRENCY_FIX.md
│   │   ├── UNDO_FLASHCARD_SYSTEM.md
│   │   ├── UNDO_FIX.md
│   │   ├── SENTENCE_DELIMITER_FIX.md
│   │   ├── IMPLEMENTATION_SUMMARY.md
│   │   └── FINAL_SUMMARY.md
│   └── guides/                  # User guides
│       ├── SYSTEM_FEATURES.md
│       ├── QUICK_REFERENCE.md
│       ├── NETWORK_DEPLOYMENT.md
│       ├── MULTIPLE_SS_GUIDE.md
│       ├── TESTING_CONCURRENT_FEATURES.md
│       └── TEST_SUITE_README.md
├── src/                         # Source code
├── tests/                       # Test scripts
└── bin/                         # Compiled binaries (after build)
```

## Quick Command Reference

```bash
# Build the project
make clean && make

# Run naming server
./bin/nm 8080

# Run storage server
./bin/ss localhost 8080 9001 ~/storage

# Run client
./bin/client localhost 8080

# Run tests
./tests/all_tests.sh
```

## Production Ready Features

✅ Natural punctuation in text (`.!?` allowed)  
✅ Concurrent write support (multiple users, different sentences)  
✅ Lost update prevention (optimistic concurrency control)  
✅ Per-session undo with unlimited depth  
✅ Comprehensive test suite (45 automated tests)  
✅ Complete documentation  
✅ Network deployment support  
✅ Multiple storage server support  

## Support

For issues or questions:
1. Check [TROUBLESHOOTING.md](guides/TESTING_CONCURRENT_FEATURES.md) section
2. Review [CRITICAL_FIXES.md](fixes/CRITICAL_FIXES.md) for known issues
3. Run test suite to validate setup: `./tests/all_tests.sh`
