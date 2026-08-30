/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MACHINE_DISK_H
#define MACHINE_DISK_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/BufferView.h"

class String;

/**
 * A disk is a random access fixed size block device.
 */
class EXPORTED_PUBLIC Disk : public Device {
 public:
  enum SubType { ATA = 0, ATAPI };

  Disk();
  Disk(Device* p);
  virtual ~Disk();

  virtual Type getType();

  virtual SubType getSubType();

  virtual void getName(String& str);

  virtual void dump(String& str);

  /**
   * Read from \p location on disk and return a view beginning at that byte.
   * \p location must be 512-byte aligned. The returned view ends at the
   * boundary of the independently owned cache page containing \p location;
   * its size is unrelated to the device's block or readahead size.
   * \note A successful read returns exactly one caller-owned reference to the
   *       cache page containing \p location. The view is non-owning: use
   *       unpin() exactly once after every copy and subview is no longer in
   *       use. Larger I/O extents do not extend this ownership.
   * \param location The offset from the start of the device, in bytes,
   *        to start the read, must be multiple of 512.
   * \return A writable view containing the data, or an empty view on failure.
   *         If the data is written, the page is marked as dirty and may be
   *         written back to disk at any time (or forced with \c write() or
   *         \c flush() ).
   */
  virtual BufferView read(uint64_t location);

  /**
   * Reads exactly the requested length into a caller-supplied sequence of bounded
   * cache views. The sequence must be empty on entry. Each appended view owns
   * one cache reference until unpinViews() is called.
   *
   * Failure releases every reference acquired by this operation and leaves
   * the sequence empty.
   */
  MUST_USE_RESULT bool readViews(uint64_t location, size_t length, BufferViewSequence& views);

  /** Releases every cache reference represented by the supplied sequence. */
  void unpinViews(uint64_t location, BufferViewSequence& views);

  /**
   * This function schedules a cache writeback of the given location.
   * The data to be written back is fetched from the cache (pointer returned
   * by \c read() ).
   * \param location The offset from the start of the device, in bytes, to
   *                 start the write. Must be 512byte aligned.
   */
  virtual void write(uint64_t location);

  /**
   * \brief Sets the page boundary alignment after a specific location on the
   * disk.
   *
   * For example, a partition beginning at byte 512 should align its first
   * cache page with that boundary rather than with byte zero of the parent.
   *
   * Use this function to keep the cache pages manipulated in \c read() and
   * \c write() aligned with a child device whose start is not naturally
   * page-aligned on the parent.
   */
  virtual void align(uint64_t location);

  /**
   * \brief Gets the size of the disk.
   *
   * This is the size in bytes of the disk. Reads or writes beyond this size
   * will fail.
   */
  virtual size_t getSize() const;

  /**
   * \brief Gets the preferred I/O extent of the disk.
   *
   * This may describe a native device block or a larger cache-fill/readahead
   * extent, depending on the implementation. It is independent of the cache
   * page boundary reported by BufferView::size().
   */
  virtual size_t getBlockSize() const;

  /**
   * \brief Pins a cache page.
   *
   * This allows an upstream user of Disk pages to 'pin' cache pages, causing
   * them to only be freed once all consumers have done an 'unpin'. The pin
   * and unpin semantics allow for memory mappings to be made in a reasonably
   * safe manner, as it can be assumed that the physical page for a particular
   * cache block will not be freed.
   *
   * \return True only when this call acquired a reference to the page
   * currently published for \p location. Callers must not use an address
   * obtained before a failed pin.
   */
  MUST_USE_RESULT virtual bool pin(uint64_t location) = 0;

  /**
   * Unpins a cache page (see \c pin() for more information and rationale).
   */
  virtual void unpin(uint64_t location) = 0;

  /**
   * \brief Whether or not the cache is critical and cannot be flushed or
   * deleted.
   *
   * Some implementations of this class may provide a Disk that does not
   * actually back onto a writable media, or perhaps sit only in RAM and have
   * no correlation to physical hardware. If cache pages are deleted for these
   * implementations, data may be lost.
   *
   * Note that cache should only be marked "critical" if it is possible to
   * write via an implementation. There is no need to worry about cache pages
   * being deleted on a read-only disk as they will be re-created on the next
   * read (and no written data is lost).
   *
   * This function allows callers that want to delete cache pages to verify
   * that the cache is not critical to the performance of the implementation.
   *
   * \return True if the cache is critical and must not be removed or flushed.
   * False otherwise.
   */
  virtual bool cacheIsCritical();

  /**
   * \brief Flush a cached page to disk.
   *
   * Essentially a no-op if the given location is not actually in
   * cache. Called either by filesystem drivers (on removable disks) or from a
   * central cache manager which handles flushing caches back to the disk on a
   * regular basis.
   *
   * Will not remove the page from cache, that must be done by the caller.
   */
  virtual void flush(uint64_t location);

  /**
   * Synchronously writes and retires the target cache page containing
   * \p location.
   *
   * A successful call means the page is no longer published by this disk's
   * cache. It does not imply that a volatile device write cache has been
   * flushed. Callers must release their own reference to the page before
   * entering this operation.
   *
   * \return True when the page was retired or was already absent. False when
   *         this disk does not support retirement or the page must remain
   *         available for a later retry.
   */
  MUST_USE_RESULT virtual bool retireCachePage(uint64_t location);
};

#endif
