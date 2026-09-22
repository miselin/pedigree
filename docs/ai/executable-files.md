# Updating running programs and libraries

Install a replacement into a temporary file in the destination directory, finish
writing its contents and metadata, then rename it over the destination. Existing
processes keep the old inode, including pages they have not faulted in yet. New
opens and executions see the replacement. The old inode is reclaimed after its
last reference disappears.

PUP uses this sequence through `InstallTarFile.makefile` in the `pedigree-apps`
repository. Older PUP versions that truncate the destination must be updated
before installing libraries they use themselves, such as zlib.

The kernel rejects nonempty writes, truncation, and allocation against a file
used by a private executable mapping with `ETXTBSY` (Text file busy). This covers
shared libraries as well as main executables, hard-link aliases, and descriptors
opened before the file became executable. Opening a writable descriptor alone
remains permitted; the protection applies when its contents would be changed.
Atomic rename and unlink remain available.

Protection starts before the kernel validates an ELF image and persists through
its executable mappings. Private `mmap(PROT_EXEC)` and later `mprotect(PROT_EXEC)`
also acquire protection. Fork, mapping splits, and remapping preserve it. Once a
mapping has been executable, dropping execute permission does not release its
protection; unmapping its remaining fragments does.

A file cannot simultaneously back a private executable mapping and a shared
mapping with permission to become writable. Admission in either direction fails
with `ETXTBSY`, including an execute upgrade through `mprotect`. A conflicting
`MAP_FIXED` replacement also fails; unmap the conflicting mapping first.
Ordinary non-executable private mappings retain their existing write and truncate
semantics. Explicit shared executable memory remains mutable when there is no
private executable mapping of the same backing.

The protection counts belong to the backing cache shared by filesystem aliases.
Admission and file mutation use the same data lock, so a write or truncate cannot
pass its check while executable ownership is being published. Shared mappings
register their maximum write capability because stores through writable PTEs do
not enter the file write path.

## Verification

Run `executable-file-contract-test /tmp` for ramfs and
`executable-file-contract-test /` for ext2 on a disposable writable guest disk.
Both must finish with `EXECUTABLE-FILE-CONTRACT: END PASS`. The suite checks
mutation rejection, hard links, fork and split lifetimes, permission upgrades,
shared mapping conflicts, ordinary mapping controls, and replacement of a loaded
shared library with subsequent code/data faults and a fresh load of the new file.

Run `execveat-contract-test` for running-executable replacement and
`file-resize-contract-test` for the existing non-executable mapping contracts.
