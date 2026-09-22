# Secure randomness on older machines

`getrandom`, `/dev/random`, `/dev/urandom` and executable `AT_RANDOM` use a
shared ChaCha20 generator. It obtains its initial 256-bit key from RDRAND or
RDSEED when available. Otherwise, a privileged initializer must supply 32
bytes from a trusted entropy source. It never substitutes clocks or the
kernel's noncryptographic PRNG. Before initialization, random-byte requests
fail with `EAGAIN`, including currently blocking `getrandom` requests.

The generator replaces its secret key before returning each block of up to
32 bytes. Unused output is discarded, and temporary key material is erased.
Further trusted seeds are mixed with the existing key through HMAC-SHA256.
The root-only `RNDADDENTROPY` ioctl accepts a `rand_pool_info` request with
exactly 256 credited bits and 32 payload bytes. Ordinary device writes consume and discard the input without
crediting entropy or making an unseeded generator ready. This interface trusts root's assertion about the source.

## Persistent seed

`random-seed` initializes a machine without CPU randomness from
`/var/lib/pedigree/random-seed`. Create that file **separately for each
installation from rescue Linux**, while the installed root is mounted:

```sh
install -d -o 0 -g 0 -m 700 /mnt/pedigree-root/var/lib/pedigree
umask 077
dd if=/dev/random of=/mnt/pedigree-root/var/lib/pedigree/random-seed \
  bs=32 count=1 iflag=fullblock status=none
chmod 600 /mnt/pedigree-root/var/lib/pedigree/random-seed
chown 0:0 /mnt/pedigree-root/var/lib/pedigree/random-seed
sync
```

The file must be a root-owned regular file with one link, exactly 32 bytes,
and mode `0600`. Its parent directory must be root-owned with mode `0700`.
Keep it secret; do not include it in a shared image or source checkout.

Run `/usr/bin/random-seed` early during startup, before SSH and other users
of secure randomness. `random-seed --check` reports whether the kernel is
ready without exposing random bytes.

The initializer holds a persistent advisory lock while reading and advancing
the saved seed. Two domain-separated HMAC-SHA256 derivations produce the
kernel seed and its on-disk successor. It writes and fsyncs an exclusive
`.next` file, renames it over the original, and fsyncs the parent directory.
Only after those operations succeed does it initialize the kernel. A crash
after persistence but before initialization skips an unused generation.

This requires disk writes enabled and storage that honors flush commands.
It does not make ext2 transactional or protect against storage rollback.
Before booting a clone or restored snapshot, replace its saved seed with
fresh rescue-Linux randomness; separately replace cloned SSH host keys.

An interrupted transaction can leave `random-seed.next`, or ext2 damage.
The initializer fails closed. Recover from rescue Linux: check the unmounted
filesystem, mount it, generate a fresh seed as above, remove a leftover
`random-seed.next`, sync and unmount. Do not delete the lock pathname during
normal operation or recover concurrently with a running initializer.

No IRQ entropy collector, TPM entropy source or firmware RNG handoff is
provided by this implementation. A machine without CPU randomness depends
on the secrecy and freshness of its rescue-generated seed.
