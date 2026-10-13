# Image format and implementation boundary

This is an original format. No course filesystem source or layout is consulted.
The implementation is in `fs/`. It runs in user space and stores its blocks in
one regular file. It does not implement a kernel filesystem or a journal.

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

Crash tests stop a real writer at a documented commit boundary, kill it with
SIGKILL, and run an independent read-only fsck. `OS_FS_CRASH_POINT=bitmap-written`
stops after an allocation bitmap block is written and synced. The interrupted
allocation leaves referenced metadata unchanged and allocated blocks unreferenced.
`OS_FS_CRASH_POINT=data-written` stops after one data block is written and synced.
Overwriting an existing two-block file then leaves 4,096 new bytes and 4,096 old
bytes, while structural fsck passes. These are controlled process-death tests,
not hardware power-loss or torn-sector simulations.

The checker validates geometry, bitmap ownership, block bounds, duplicate
block references, pointers beyond EOF, directory names and targets, parent links,
reachability, and link counts. It reports at most 1,000 findings and never repairs
the source image. It shares byte decoding with the writer but independently walks
the pointer tree and directory graph. It does not use Image's allocation or path
code. Content without a checksum can change without a structural error.

## Interfaces and validation

The library, `fs_cli`, `fs_mkfs`, `fs_check`, and `fs_mount` use the same image
format. Tests cross the direct/single/double-indirect boundaries, exercise sparse
holes and shrinking/expanding truncation, compare 500 randomized operations with
a byte-vector oracle, and cover create/read/write/unlink/mkdir/rename/hard links.
Six corruption cases check invalid pointers, duplicate blocks, wrong link counts,
invalid superblocks, referenced-but-free blocks, and unreachable inodes.

FUSE tests run in a dedicated container with `/dev/fuse` and `SYS_ADMIN`.
They use real Linux system calls, including open-file replacement and deletion,
directory moves, and 48 file round trips from four concurrent workers. FUSE uses
its default hidden-name handling for open files whose directory entries are
removed. An initial `hard_remove` configuration caused ESTALE on `fstat` after
unlink; removing that setting passed the regression. This behavior follows the
[libfuse configuration contract](https://github.com/libfuse/libfuse/blob/fuse-3.17.2/include/fuse.h).
The underlying Image library separately supports zero-link open inodes until
the last handle closes, without hidden names. A daemon crash can leave a FUSE
hidden file; fsck can find structural inconsistencies, not infer user intent.

The mounted test also found a real EOF defect: the callback passed the null data
pointer of an empty vector to `memcpy` with length zero. UBSan rejected the call's
nonnull contract and terminated the daemon. The callback now skips the copy for
an empty read. The original adapter and failing UBSan output are saved under
`results/validation/fs-eof-reproduction/` and `m3-ubsan-reproduction.log`.
The same mounted read test is the regression. This was undefined behavior, not a
data race or deadlock; it does not establish those separate concurrency claims.

All Image operations serialize through one mutex. The process holds an exclusive
advisory file lock for its lifetime. Offline fsck takes a shared lock and refuses
to inspect a live writer. A write call accepts at most 1 MiB; a read accepts at
most 64 MiB. A directory holds at most 65,536 slots, and names hold at most 56
bytes. Directory hard links and rename into a descendant are rejected. Rename
to a second name for the same inode is a no-op. Inode reuse increments a generation
counter and does not occur while a library file handle still refers to that inode.

The FUSE front end is an owner-only mount with kernel permission checks and no
data cache. It snapshots directory entries per open directory handle. Its supported
scope excludes symlinks, ownership changes, extended attributes, rename flags,
journaling, and full POSIX conformance. Access time is not stored; `st_atime`
reflects modification time. Per-file physical block accounting is not exposed
(`st_blocks` is zero). The image's free-block bitmap is available through statvfs.
Negative timestamps and metadata changes through a pathless removed-file handle
are not supported. I/O errors during commit can leave partial disk changes;
transaction staging only guarantees rollback before the commit starts.

## Commands

Run these commands inside the development container or a Linux build environment:

```sh
fs_mkfs --image /tmp/example.img --blocks 8192 --inodes 256
fs_cli --image /tmp/example.img --op mkdir --path /data
fs_cli --image /tmp/example.img --op put --path /data/file --input /tmp/input.bin
fs_cli --image /tmp/example.img --op get --path /data/file --output /tmp/output.bin
fs_check --image /tmp/example.img
mkdir /tmp/mount
fs_mount /tmp/example.img /tmp/mount
# From another terminal in the same container:
fusermount3 -u /tmp/mount
```

`fs_mkfs` refuses to overwrite a file. `fs_check` exits 0 for a clean structure,
2 for findings, and 1 for invalid command-line use. `fs_mount` stays in the
foreground. CLI `write` changes an existing file at `--offset`; `put` truncates
or creates the file. A multi-chunk CLI transfer is not one atomic operation.
