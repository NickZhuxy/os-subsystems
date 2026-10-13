# Planned image format and implementation boundary

This is an original format. No course filesystem source or layout is consulted.
The following is the implementation plan for M3, not evidence that it is complete.

## Format

All integers use explicit little-endian serialization. Blocks are 4,096 bytes.
Block zero holds a versioned superblock with a format identifier, geometry,
metadata region offsets, root inode number, and a header checksum. Following
regions hold an inode bitmap, a block bitmap, and a fixed-size inode table.
Metadata blocks are reserved in the block bitmap.

Inodes are 128 bytes and store type, permissions, link count, file length,
generation, UID/GID, timestamps, eight direct block pointers, one single-indirect
pointer, and one double-indirect pointer. Pointer blocks hold 32-bit block IDs.
Zero means a sparse hole. Inode zero is reserved; inode one is the root.

Directories are ordinary inode data. Each fixed-size 64-byte record stores an
inode ID, a byte-length, and up to 56 name bytes. Directories contain `.` and `..`.
An all-zero inode ID marks an unused directory slot. Hard links to directories
are forbidden. Rename must reject a move into the directory's own descendants.

## Mutation and failure model

A mutation prepares dirty blocks in a bounded in-memory transaction. Validation
or ENOSPC errors discard the transaction. Commit writes the prepared blocks and
syncs the image. This protects against logical operation failure but provides no
on-disk journal: power loss or process death during commit can leave partial
metadata. The project must state this limitation.

Crash tests will stop a real writer at a documented commit boundary, kill it,
and run an independent read-only fsck. Allocation-bit-first ordering provides a
repeatable interruption point. Another case should show that structural fsck
success cannot establish atomic application data updates.

The checker will validate geometry, bitmap ownership, block bounds, duplicate
block references, directory names and targets, reachability, and link counts.
It will report findings, never repair the source image implicitly.

## Interfaces and validation

The library, CLI, mkfs, fsck, and FUSE front end will use the same image format.
Tests must cross the direct/single/double-indirect boundaries, exercise sparse
holes and shrinking/expanding truncation, compare bytes with an independent
oracle, and cover create/read/write/unlink/mkdir/rename/hard links. FUSE tests
will run in a dedicated container with `/dev/fuse`; the device is available in
the current Docker VM. Mount success has not yet been tested.
