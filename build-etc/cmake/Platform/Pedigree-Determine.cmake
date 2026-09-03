# Pedigree currently has one maintained native architecture. Some uname
# implementations do not provide a useful value for `uname -p`, which CMake
# consults before `uname -m` on otherwise unknown Unix platforms.
if (NOT CMAKE_SYSTEM_PROCESSOR OR
    CMAKE_SYSTEM_PROCESSOR STREQUAL "unknown" OR
    CMAKE_SYSTEM_PROCESSOR MATCHES
        "^(amd64|x64-mach_pc|x86_64-pedigree)$")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif ()
