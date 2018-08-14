
set(PEDIGREE_BUILDUTILS_ASAN OFF CACHE BOOL "Build build utilities with the address sanitizer enabled.")

include(CodeCoverage)

find_package(OpenSSL)
find_package(Threads REQUIRED)

find_library(BENCHMARK_LIBRARY benchmark)
find_path(BENCHMARK_INCLUDE_DIR benchmark/benchmark.h)

if (BENCHMARK_INCLUDE_DIR)
    include_directories(${BENCHMARK_INCLUDE_DIR})
endif ()

# TODO: should detect a few of these things
add_definitions(
    -DHOSTED -DUTILITY_LINUX -DTARGET_IS_LITTLE_ENDIAN -DDONT_LOG_TO_SERIAL
    -DPEDIGREE_BENCHMARK -DSTANDALONE_MUTEXES -DSTANDALONE_CACHE
    -DSTANDALONE_MEMPOOL -DDEVICE_IGNORE_ADDRESSES -DBITS_64 -DLITTLE_ENDIAN
    -DVFS_NOMMU -DVFS_STANDALONE -DEXT2_STANDALONE -DADDITIONAL_CHECKS)

set(BUILDUTIL_FLAGS -ggdb -gdwarf-2)
set(BUILDUTIL_CFLAGS ${BUILDUTIL_FLAGS} ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CFLAGS})
set(BUILDUTIL_CXXFLAGS ${BUILDUTIL_FLAGS} ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CXXFLAGS})
set(BUILDUTIL_LINKFLAGS "")

if (PEDIGREE_BUILDUTILS_ASAN)
    set(BUILDUTIL_CFLAGS ${BUILDUTIL_CFLAGS} -fsanitize=address)
    set(BUILDUTIL_CXXFLAGS ${BUILDUTIL_CXXFLAGS} -fsanitize=address)
    set(BUILDUTIL_LINKFLAGS "${BUILDUTIL_LINKFLAGS} -fsanitize=address")
endif ()

set(NONCOVERAGE_FLAGS -Os)
set(COVERAGE_FLAGS -O0 -fprofile-arcs -ftest-coverage)
set(COVERAGE_LINKFLAGS --coverage gcov)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${BUILDUTIL_LINKFLAGS}")

add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:${BUILDUTIL_CFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${BUILDUTIL_CXXFLAGS}>")

if (OPENSSL_FOUND)
    add_definitions(-DHAVE_OPENSSL)
endif ()

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/system/include
)

set(KERNEL_SRCS
    ${CMAKE_SOURCE_DIR}/src/system/kernel/machine/Device.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/machine/DeviceHashTree.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/machine/Disk.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/linker/SymbolTable.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/IoBase.cc)
add_library(kernel ${KERNEL_SRCS})
add_library(kernel_coverage ${KERNEL_SRCS})
target_compile_options(kernel PRIVATE ${NONCOVERAGE_FLAGS})
# NOTE: using 'PUBLIC' here means anything using kernel_coverage will also get
# these flags.
target_compile_options(kernel_coverage PUBLIC ${COVERAGE_FLAGS})
target_link_libraries(kernel_coverage PUBLIC ${COVERAGE_FLAGS} ${COVERAGE_LINKFLAGS})
target_compile_definitions(kernel_coverage PUBLIC -DUTILITY_LINUX_COVERAGE)

set(UTILITY_SRCS
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/BloomFilter.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/Buffer.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/Cache.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/ExtensibleBitmap.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/HashTable.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/LazyEvaluate.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/List.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/LruCache.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/ObjectPool.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/ProducerConsumer.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/RadixTree.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/RangeList.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/RequestQueue.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/Result.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/RingBuffer.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/SharedPointer.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/StaticString.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/String.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/StringView.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/Tree.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/UnlikelyLock.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/utility.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/Vector.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/sha1/sha1.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/utilities/smhasher/MurmurHash3.cpp
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/lib/demangle.cc
    # Optimized overrides here.
    # TODO(miselin): handle non-x86 host OS.
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/x86_common/string.c
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/lib/string.c
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/lib/memory.c
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/lib/vsprintf.c
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/lib/SlamAllocator.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/PhysicalMemoryManager.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/time/Conversion.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/time/Concurrent.cc  # Portable, unlike Delay.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/network/IpAddress.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/network/MacAddress.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/Atomic.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/Log.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/LockGuard.cc

    ${CMAKE_CURRENT_SOURCE_DIR}/shim.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/pocketknife.cc)

add_library(utility ${UTILITY_SRCS})
add_library(utility_coverage ${UTILITY_SRCS})
target_compile_options(utility PRIVATE ${NONCOVERAGE_FLAGS})
target_compile_options(utility_coverage PUBLIC ${COVERAGE_FLAGS})
target_link_libraries(utility_coverage PUBLIC ${COVERAGE_FLAGS} ${COVERAGE_LINKFLAGS})
target_compile_definitions(utility_coverage PUBLIC -DUTILITY_LINUX_COVERAGE)

add_library(vfs
    ${CMAKE_SOURCE_DIR}/src/modules/system/vfs/Directory.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/vfs/File.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/vfs/Filesystem.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/vfs/Symlink.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/vfs/VFS.cc
)

add_library(ext2
    ${CMAKE_SOURCE_DIR}/src/modules/system/ext2/Ext2Directory.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/ext2/Ext2File.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/ext2/Ext2Filesystem.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/ext2/Ext2Node.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/ext2/Ext2Symlink.cc
)

add_library(ramfs
    ${CMAKE_SOURCE_DIR}/src/modules/system/ramfs/RamFs.cc
)

add_library(partition
    ${CMAKE_SOURCE_DIR}/src/modules/drivers/common/partition/Partition.cc
    ${CMAKE_SOURCE_DIR}/src/modules/drivers/common/partition/apple.cc
    ${CMAKE_SOURCE_DIR}/src/modules/drivers/common/partition/msdos.cc)

add_library(debugger
    ${CMAKE_SOURCE_DIR}/src/system/kernel/debugger/Scrollable.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/debugger/DebuggerCommand.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/debugger/DebuggerIO.cc
    ${CMAKE_SOURCE_DIR}/src/system/kernel/debugger/commands/LocksCommand.cc)
target_compile_definitions(debugger PRIVATE -DTESTSUITE)

add_library(config
    ${CMAKE_SOURCE_DIR}/src/modules/system/config/Config.cc
    ${CMAKE_SOURCE_DIR}/src/modules/system/config/sqlite3/sqlite3.c)

add_executable(keymap
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/main.c)
target_compile_definitions(keymap PRIVATE -DNOT_PEDIGREE)
target_include_directories(keymap PRIVATE
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap)
target_link_libraries(keymap PRIVATE libkeymap)

add_executable(ext2img
    ext2img/DiskImage.cc
    ext2img/main.cc
    ext2img/stubs.cc)
target_link_libraries(ext2img PRIVATE
    ext2 vfs partition utility kernel
    $<$<BOOL:OPENSSL_FOUND>:${OPENSSL_CRYPTO_LIBRARY}> Threads::Threads)

add_executable(instrument instrument/main.cc)
target_include_directories(instrument PRIVATE
    ${CMAKE_SOURCE_DIR}/src/system/kernel/core)

add_executable(memorytracer memorytracer/main.cc)
target_compile_definitions(memorytracer PRIVATE -DMEMORY_TRACING)

# TODO: figure out how to build vdsotest given vdso.h could potentially not exist

add_executable(testsuite
    testsuite/test-BloomFilter.cc
    testsuite/test-Tree.cc
    testsuite/test-ObjectPool.cc
    testsuite/test-SlamAllocator.cc
    testsuite/test-utility.cc
    testsuite/test-String.cc
    testsuite/test-Vector.cc
    testsuite/test-memory.cc
    testsuite/test-Result.cc
    testsuite/test-RangeList.cc
    testsuite/test-LocksCommand.cc
    testsuite/test-List.cc
    testsuite/test-SharedPointer.cc
    testsuite/test-UniquePointer.cc
    testsuite/test-Buffer.cc
    testsuite/test-stringlib.cc
    testsuite/test-ExtensibleBitmap.cc
    testsuite/test-Time.cc
    testsuite/test-SymbolTable.cc
    testsuite/test-RadixTree.cc
    testsuite/test-HashTable.cc
    testsuite/test-StaticString.cc
    testsuite/test-LazyEvaluate.cc
    testsuite/test-ConditionVariable.cc
    testsuite/test-StringView.cc
    testsuite/test-LruCache.cc
    testsuite/test-Log.cc
)
target_link_libraries(testsuite PRIVATE
    kernel_coverage debugger vfs utility_coverage Threads::Threads gtest gtest_main)
target_compile_definitions(testsuite PRIVATE -DTESTSUITE)
target_compile_options(testsuite PRIVATE ${COVERAGE_FLAGS})
target_link_libraries(testsuite PRIVATE ${COVERAGE_FLAGS} ${COVERAGE_LINKFLAGS})

export(
    TARGETS keymap ext2img instrument memorytracer testsuite
    FILE ${CMAKE_BINARY_DIR}/HostUtilities.cmake NAMESPACE host-
)

if (BENCHMARK_LIBRARY)
    add_executable(benchmarker
        testsuite/bench-BloomFilter.cc
        testsuite/bench-ExtensibleBitmap.cc
        testsuite/bench-SymbolTableConcepts.cc
        testsuite/bench-stringlib.cc
        testsuite/bench-RangeList.cc
        testsuite/bench-HashTable.cc
        testsuite/bench-List.cc
        testsuite/bench-SlamAllocator.cc
        testsuite/bench-main.cc
        testsuite/bench-DirectoryStructures.cc
        testsuite/bench-RadixTree.cc
        testsuite/bench-StaticString.cc
        testsuite/bench-Vector.cc
        testsuite/bench-utility.cc
        testsuite/bench-String.cc
        testsuite/bench-memory.cc
        testsuite/bench-Tree.cc
        testsuite/bench-VFS.cc
        testsuite/bench-LruCache.cc
        testsuite/bench-Log.cc
    )
    target_link_libraries(benchmarker PRIVATE
        kernel ramfs vfs utility Threads::Threads ${BENCHMARK_LIBRARY})
    target_compile_options(benchmarker PRIVATE "-Os" "-march=native" "-mtune=native")
    target_compile_definitions(benchmarker PRIVATE -DTESTSUITE)

    export(
        TARGETS benchmarker
        APPEND FILE ${CMAKE_BINARY_DIR}/HostUtilities.cmake NAMESPACE host-
    )
endif ()

# TODO: build netwrap

SETUP_TARGET_FOR_COVERAGE(
    NAME testsuite_coverage
    EXECUTABLE $<TARGET_FILE:testsuite>
    DEPENDENCIES testsuite)

add_test(NAME testsuite COMMAND $<TARGET_FILE:testsuite>)
