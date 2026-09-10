cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/PedigreeMuslSdkManifest.cmake")

foreach(required SDK_ROOT PUP_CACHE PUP_SERVER PUP_COMMAND)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif ()
endforeach()
foreach(path SDK_ROOT PUP_CACHE)
    if (NOT IS_ABSOLUTE "${${path}}" OR "${${path}}" STREQUAL "/")
        message(FATAL_ERROR "${path} must be an absolute build-local path")
    endif ()
endforeach()

set(package "musl-1.2.6-amd64.pup")
set(sha256 "e41cb870e18e68260c76415bf49296c3e78e6e613ac846591097fe09eb737c40")
file(MAKE_DIRECTORY "${PUP_CACHE}")
file(LOCK "${PUP_CACHE}/install.lock" GUARD PROCESS TIMEOUT 120)

set(marker "${SDK_ROOT}/usr/share/pedigree/libc/package.sha256")
if (EXISTS "${marker}")
    file(READ "${marker}" installed)
    if (installed STREQUAL sha256)
        execute_process(
            COMMAND ${CMAKE_COMMAND} "-DSDK_ROOT=${SDK_ROOT}"
                -P "${CMAKE_CURRENT_LIST_DIR}/ValidateMuslPackage.cmake"
            RESULT_VARIABLE valid OUTPUT_QUIET ERROR_QUIET)
        if (valid EQUAL 0)
            return()
        endif ()
    endif ()
endif ()

# PUP resolves the latest entry in its database. A private, pinned entry avoids
# changing the libc ABI implicitly when the public repository publishes a release.
file(WRITE "${PUP_CACHE}/packages.pupdb"
    "{\"musl-amd64\":{\"name\":\"musl\",\"version\":\"1.2.6\",\"architecture\":\"amd64\",\"sha1\":\"ce01ee8e58d85c1669026d94ecd29379dacc2ad8\",\"dependencies\":[]}}\n")
set(stage "${SDK_ROOT}.staging")
file(REMOVE_RECURSE "${stage}")
file(WRITE "${PUP_CACHE}/pup.conf"
    "[paths]\ninstallroot=${stage}\nlocaldb=${PUP_CACHE}\n[settings]\narch=amd64\n[remotes]\nserver=${PUP_SERVER}\n")
execute_process(
    COMMAND ${PUP_COMMAND} "--config=${PUP_CACHE}/pup.conf" install musl
    RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "pup install musl failed (${result})")
endif ()
file(SHA256 "${PUP_CACHE}/${package}" actual)
if (NOT actual STREQUAL sha256)
    message(FATAL_ERROR "The downloaded musl PUP does not match the pinned SHA256")
endif ()
pedigree_validate_musl_sdk_root(
    ROOT "${stage}" ARCHITECTURE x86_64 LAYOUT fhs-usr-v1)
file(MAKE_DIRECTORY "${stage}/usr/share/pedigree/libc")
file(WRITE "${stage}/usr/share/pedigree/libc/package.sha256" "${sha256}")
file(REMOVE_RECURSE "${SDK_ROOT}.previous")
if (EXISTS "${SDK_ROOT}")
    file(RENAME "${SDK_ROOT}" "${SDK_ROOT}.previous")
endif ()
file(RENAME "${stage}" "${SDK_ROOT}" RESULT published)
if (NOT published STREQUAL "0")
    if (EXISTS "${SDK_ROOT}.previous")
        file(RENAME "${SDK_ROOT}.previous" "${SDK_ROOT}")
    endif ()
    message(FATAL_ERROR "Could not publish the musl SDK: ${published}")
endif ()
file(REMOVE_RECURSE "${SDK_ROOT}.previous")
