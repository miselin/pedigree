import os
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CXX = shlex.split(os.environ.get("CXX", "c++"))
FIXTURE = ROOT / "tests/fixtures/fallible-vm-metadata.cc"

# A separate translation unit keeps the nullable allocator opaque to the
# container instantiations, as it is in the kernel's compiled callers.
ALLOCATOR = r"""
#include <cstddef>
#include <cstdlib>

static long remaining = -1;
static long attempts = 0;
static long live = 0;

extern "C" void allocation_limit(long count) {
  remaining = count;
  attempts = 0;
}
extern "C" long allocation_attempts() { return attempts; }
extern "C" long live_allocations() { return live; }

static void* allocate(std::size_t size) {
  ++attempts;
  if (remaining == 0)
    return nullptr;
  if (remaining > 0)
    --remaining;
  void* result = std::malloc(size ? size : 1);
  if (result)
    ++live;
  return result;
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }

static void release(void* pointer) {
  if (pointer) {
    --live;
    std::free(pointer);
  }
}
void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { release(pointer); }
"""


class FallibleVmMetadataTests(unittest.TestCase):
    @unittest.skipUnless(
        CXX and shutil.which(CXX[0]), "requires a native C++ compiler"
    )
    def test_nullable_new_preserves_metadata_and_constructor_lifetimes(self):
        with tempfile.TemporaryDirectory(
            prefix="pedigree-fallible-vm-"
        ) as temporary:
            directory = Path(temporary)
            (directory / "config.h").write_text(
                textwrap.dedent(
                    """\
                    #define HOSTED 1
                    #define THREADS 0
                    #define UTILITY_LINUX 1
                    #define PEDIGREE_TARGET_PAGE_SIZE 4096
                    #define TARGET_IS_LITTLE_ENDIAN 1
                    """
                )
            )
            allocator = directory / "allocator.cc"
            allocator.write_text(ALLOCATOR)
            executable = directory / "fallible-vm-metadata"
            command = [
                *CXX,
                "-std=c++23",
                "-O2",
                "-fno-exceptions",
                "-fcheck-new",
                "-fno-lto",
                "-I",
                str(directory),
                "-I",
                str(ROOT / "src/system/include"),
                str(FIXTURE),
                str(allocator),
                "-o",
                str(executable),
            ]
            compiled = subprocess.run(
                command, capture_output=True, text=True, timeout=60
            )
            self.assertEqual(
                compiled.returncode,
                0,
                msg=shlex.join(command) + "\n" + compiled.stdout + compiled.stderr,
            )
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(
                result.returncode, 0, msg=result.stdout + result.stderr
            )
            self.assertEqual(
                result.stdout.splitlines(),
                [
                    "FALLIBLE-VM-METADATA: PASS tree",
                    "FALLIBLE-VM-METADATA: PASS list",
                    "FALLIBLE-VM-METADATA: PASS vector",
                    "FALLIBLE-VM-METADATA: PASS ranges",
                    "FALLIBLE-VM-METADATA: END PASS",
                ],
            )


if __name__ == "__main__":
    unittest.main()
