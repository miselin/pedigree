
set(PEDIGREE_VERBOSE_KERNEL FALSE CACHE BOOL "Enable verbose logging throughout the kernel.")
set(PEDIGREE_DEBUG_ALLOCATOR FALSE CACHE BOOL "Replace SLAM with a debug allocator which never re-uses allocations (it is highly recommended to only use this on 64-bit targets).")

# TODO: x64-specific
set(GENERIC_KERNEL_FLAGS
    "-fno-builtin" "-nostdlib" "-ffreestanding" "-fno-common" "-m64"
    "-mno-red-zone" "-march=k8" "-mno-sse" "-mno-mmx" "-fno-omit-frame-pointer"
    "-ggdb" "-g3" "-gdwarf-2" "--sysroot=${CMAKE_SYSROOT}")
set(GENERIC_KERNEL_CFLAGS "")
set(GENERIC_KERNEL_CXXFLAGS
    "-fno-exceptions" "-fno-rtti" "-fno-asynchronous-unwind-tables"
    "-fno-use-cxa-atexit")
set(GENERIC_KERNEL_LINKFLAGS
    "-nostdlib" "-nostartfiles" "-m64" "-Wl,--discard-all" "-static-libgcc" "--sysroot=${CMAKE_SYSROOT}")
set(GENERIC_KERNEL_LINKFLAGS_NOLINKSCRIPT ${GENERIC_KERNEL_LINKFLAGS})
if (PEDIGREE_LINKERSCRIPT)
    set(GENERIC_KERNEL_LINKFLAGS
        ${GENERIC_KERNEL_LINKFLAGS} "-T${PEDIGREE_LINKERSCRIPT}")
endif ()

set(KERNEL_CFLAGS
    ${GENERIC_CFLAGS} ${GENERIC_KERNEL_FLAGS} ${GENERIC_KERNEL_CFLAGS})
set(KERNEL_CXXFLAGS
    ${GENERIC_CXXFLAGS} ${GENERIC_KERNEL_FLAGS} ${GENERIC_KERNEL_CXXFLAGS})

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Version.cc.in
    ${CMAKE_CURRENT_BINARY_DIR}/Version.cc)

add_definitions(
    -DIN_PEDIGREE_KERNEL -DDEBUGGER_QWERTY -DKERNEL_STANDALONE)

if (PEDIGREE_VERBOSE_KERNEL)
    add_definitions(-DVERBOSE_KERNEL)
endif ()

if (PEDIGREE_DEBUG_ALLOCATOR)
    add_definitions(-DSLAM_USE_DEBUG_ALLOCATOR)
endif ()

add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:${KERNEL_CFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${KERNEL_CXXFLAGS}>")

if (PEDIGREE_ARCHDIR STREQUAL "x64")
    set(PROCESSOR_UNSHARED_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/boot-standalone.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/trampoline.s)

    set(PROCESSOR_SRCS
        # /core/processor/x64/
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/Elf.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/gdt.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/InterruptManager.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/Multiprocessor.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/NMFaultHandler.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/PageFaultHandler.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/Processor.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/StackFrame.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/state.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/SyscallManager.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/VirtualAddressSpace.cc
        # /core/processor/x64/asm/
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/Processor.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/instrument.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/gdt.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/SyscallManager.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/InterruptManager.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/Scheduler.s
        # /core/processor/x86_common/
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/Disassembler.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/IoPort.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/Multiprocessor.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/PhysicalMemoryManager.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/Processor.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/ProcessorInformation.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x86_common/string.c
        # /core/lib/x64
        ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/x64/fastmemory.s)

    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/Processor.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/instrument.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/gdt.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/SyscallManager.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/trampoline.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/InterruptManager.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/boot-standalone.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/x64/asm/Scheduler.s
        ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/x64/fastmemory.s
        PROPERTIES LANGUAGE ASM_NASM)
else ()
    set(PROCESSOR_SRCS "")
    set(PROCESSOR_UNSHARED_SRCS "")
endif ()

if (PEDIGREE_MACHDIR STREQUAL "mach_pc")
    set(MACHINE_SRCS
        # /machine/mach_pc/
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Acpi.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Apic.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Bios.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/IoApic.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Keyboard.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/LocalApic.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Machine.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Pc.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Pci.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Pic.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Pit.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Ps2Controller.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Rtc.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Serial.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/SMBios.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Smp.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Trace.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/Vga.cc)

    set(X86EMU_SRCS
        # /machine/mach_pc/
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/prim_ops.c
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/ops2.c
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/fpu.c
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/ops.c
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/sys.c
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/decode.c
        ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/debug.c)

    if (PEDIGREE_MULTIPROCESSOR)
        add_definitions(-DAPIC -DSMP)
    endif ()
else()
    set(MACHINE_SRCS "")
    set(X86EMU_SRCS "")
endif ()

set(UDIS86_SRCS
    # /debugger/
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/mnemonics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/udis86.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/opcmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/decode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/syn-intel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/syn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/libudis86/syn-att.c)

set(HASH_SRCS
    # /utilities/sha1/
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/sha1/sha1.cc
    # /utilities/md5/
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/md5/md5.cc
    # /utilities/smhasher/
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/smhasher/MurmurHash3.cpp
    # /utilities/spooky/
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/spooky/spooky.cc)

set(KERNEL_SRCS
    # /
    ${CMAKE_CURRENT_BINARY_DIR}/Version.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Archive.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Atomic.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/LockGuard.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Log.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/SerialLog.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ServiceFeatures.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ServiceManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Spinlock.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Subsystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/syscallError.cc
    # /config/
    ${CMAKE_CURRENT_SOURCE_DIR}/config/ConfigurationBackend.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/config/ConfigurationManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/config/MemoryBackend.cc
    # /core/
    ${CMAKE_CURRENT_SOURCE_DIR}/core/BootIO.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/BootstrapInfo.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/KernelCoreSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/main.cc
    # /core/process/
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/ConditionVariable.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Event.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/InfoBlock.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/initialiseMultitasking.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Ipc.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/LockManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/MemoryPressureKiller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/MemoryPressureManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Mutex.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/PerProcessorScheduler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Process.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/ProcessorThreadAllocator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/RoundRobin.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/RoundRobinCoreAllocator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Scheduler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/SchedulingAlgorithm.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Semaphore.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/SignalEvent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Thread.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/ThreadToCoreAllocationAlgorithm.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/TimeTracker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process/Uninterruptible.cc
    # /core/processor/
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/Disassembler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/InterruptHandler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/InterruptManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/IoBase.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/IoPort.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/IoPortManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/MemoryMappedIo.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/MemoryRegion.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/PageFaultHandler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/PhysicalMemoryManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/Processor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/StackFrameBase.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/SyscallHandler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/SyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/processor/VirtualAddressSpace.cc
    # /core/lib/
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/cppsupport.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/DebugAllocator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/demangle.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/instrument.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/SlamAllocator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/lib/vsprintf.c
    # /debugger/
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/Backtrace.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/Debugger.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/DebuggerCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/DebuggerIO.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/DwarfCfiAutomaton.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/DwarfUnwinder.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/LocalIO.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/Scrollable.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/SerialIO.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/SyscallTracer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/assert.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/panic.cc
    # /debugger/commands/
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/AllocationCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/Backtracer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/BreakpointCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/CpuInfoCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/DevicesCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/DisassembleCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/DumpCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/HelpCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/IoCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/LocksCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/LogViewer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/LookupCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/MappingCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/MemoryInspector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/PanicCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/QuitCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/SlamCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/StepCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/SyscallTracerCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/ThreadsCommand.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debugger/commands/TraceCommand.cc
    # /graphics/
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Graphics.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/GraphicsService.cc
    # /linker/
    ${CMAKE_CURRENT_SOURCE_DIR}/linker/Elf.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/linker/KernelElf.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/linker/SymbolTable.cc
    # /machine/
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Bus.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Device.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/DeviceHashTree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Disk.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Display.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Framebuffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/HidInputManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/InputManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/IrqHandler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/IrqManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Keyboard.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/KeymapManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Machine.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Network.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/SchedulerTimer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Serial.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Timer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/TimerHandler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/Vga.cc
    # /network/
    ${CMAKE_CURRENT_SOURCE_DIR}/network/IpAddress.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/network/MacAddress.cc
    # /time/
    ${CMAKE_CURRENT_SOURCE_DIR}/time/Concurrent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time/Conversion.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time/Delay.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time/Stopwatch.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time/Time.cc
    # /utilities/
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/BloomFilter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/Buffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/Cache.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/Cord.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/ExtensibleBitmap.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/HashTable.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/LazyEvaluate.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/List.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/LruCache.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/MemoryCount.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/MemoryPool.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/ObjectPool.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/pocketknife.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/ProducerConsumer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/RadixTree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/RangeList.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/RequestQueue.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/Result.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/RingBuffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/SharedPointer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/StaticCord.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/StaticString.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/String.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/StringView.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/TimeoutGuard.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/Tree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/UnlikelyLock.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/utility.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/Vector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities/ZombieQueue.cc)

# TODO: no-debugger builds need to not pull in quite as much debugger stuff

add_library(kernel_shared SHARED
    ${KERNEL_SRCS} ${PROCESSOR_SRCS} ${MACHINE_SRCS} ${UDIS86_SRCS} ${X86EMU_SRCS} ${HASH_SRCS} ${CMAKE_CURRENT_SOURCE_DIR}/sharedshim.cc)
target_compile_options(kernel_shared PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-w ${GENERIC_CFLAGS} ${KERNEL_CFLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>:-w ${GENERIC_CXXFLAGS} ${GENERIC_KERNEL_CXXFLAGS}>)
target_compile_definitions(kernel_shared PRIVATE -D__UD_STANDALONE__)
target_link_libraries(kernel_shared PRIVATE ${GENERIC_LINKFLAGS} ${GENERIC_KERNEL_LINKFLAGS_NOLINKSCRIPT})
target_include_directories(kernel_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:-mcmodel=kernel>"
    "$<$<COMPILE_LANGUAGE:CXX>:-mcmodel=kernel>")

add_library(machine STATIC ${MACHINE_SRCS})
# TODO: remove dependencies on this if possible
target_include_directories(machine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(processor STATIC ${PROCESSOR_SRCS} ${PROCESSOR_UNSHARED_SRCS})
# TODO: remove dependencies on this if possible
target_include_directories(processor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(x86emu STATIC ${X86EMU_SRCS})
target_include_directories(x86emu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/machine/mach_pc/x86emu/x86emu)
target_compile_options(x86emu PRIVATE -w)

add_library(udis86 STATIC ${UDIS86_SRCS})
target_compile_definitions(udis86 PRIVATE -D__UD_STANDALONE__)

add_library(hash STATIC ${HASH_SRCS})
target_compile_options(hash PRIVATE -w)

# We link whole-archive for the kernel sources
add_executable(kernel ${KERNEL_SRCS})
target_link_libraries(kernel PRIVATE 
    ${GENERIC_LINKFLAGS} ${GENERIC_KERNEL_LINKFLAGS} -Wl,--whole-archive
    machine processor hash x86emu udis86)

# split debug info into a .debug file so it's accessible but not present in the module binary itself
add_custom_command(TARGET kernel POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:kernel> ${CMAKE_CURRENT_BINARY_DIR}/kernel.debug
    COMMAND ${CMAKE_STRIP} -g $<TARGET_FILE:kernel>
    COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink="${CMAKE_CURRENT_BINARY_DIR}/kernel.debug" $<TARGET_FILE:kernel>)

add_custom_command(TARGET kernel POST_BUILD
    # OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernel-mini64
    COMMAND ${CMAKE_OBJCOPY} -I elf64-x86-64 -O elf32-i386 $<TARGET_FILE:kernel> ${CMAKE_CURRENT_BINARY_DIR}/kernel-mini64
    DEPENDS kernel)
add_custom_target(kernelfinal DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel-mini64)
