include_guard(GLOBAL)

set(PEDIGREE_MUSL_SDK_MANIFEST_SCHEMA "pedigree.musl-sdk")
set(PEDIGREE_MUSL_SDK_MANIFEST_SCHEMA_VERSION 1)

function(_pedigree_musl_sdk_require_token name value)
    if ("${value}" STREQUAL "" OR
        NOT "${value}" MATCHES "^[A-Za-z0-9_.:+-]+$")
        message(FATAL_ERROR
            "${name} must be a non-empty portable identifier: ${value}")
    endif ()
endfunction()

function(_pedigree_musl_sdk_paths architecture layout)
    if (NOT "${architecture}" STREQUAL "x86_64")
        message(FATAL_ERROR
            "unsupported musl SDK architecture: ${architecture}")
    endif ()
    if (NOT "${layout}" STREQUAL "fhs-usr-v1")
        message(FATAL_ERROR "unsupported musl SDK layout: ${layout}")
    endif ()

    set(PEDIGREE_MUSL_SDK_INCLUDE_DIR "usr/include" PARENT_SCOPE)
    set(PEDIGREE_MUSL_SDK_LIBRARY_DIR "usr/lib" PARENT_SCOPE)
    set(PEDIGREE_MUSL_SDK_LOADER
        "usr/lib/ld-musl-x86_64.so.1" PARENT_SCOPE)
endfunction()

function(_pedigree_musl_sdk_require_regular_file root relative kind)
    set(path "${root}/${relative}")
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "musl SDK ${kind} is missing: ${relative}")
    endif ()
    if (IS_DIRECTORY "${path}" OR IS_SYMLINK "${path}")
        message(FATAL_ERROR
            "musl SDK ${kind} must be a regular file: ${relative}")
    endif ()
    file(SIZE "${path}" size)
    if (size EQUAL 0)
        message(FATAL_ERROR "musl SDK ${kind} is empty: ${relative}")
    endif ()
endfunction()

function(pedigree_validate_musl_sdk_root)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "ROOT;ARCHITECTURE;LAYOUT;LOADER_TARGET_OUTPUT" "")
    foreach(required ROOT ARCHITECTURE LAYOUT)
        if (NOT DEFINED ARG_${required} OR "${ARG_${required}}" STREQUAL "")
            message(FATAL_ERROR
                "pedigree_validate_musl_sdk_root requires ${required}")
        endif ()
    endforeach()
    if (NOT IS_ABSOLUTE "${ARG_ROOT}" OR NOT IS_DIRECTORY "${ARG_ROOT}")
        message(FATAL_ERROR
            "musl SDK root must be an existing absolute directory: ${ARG_ROOT}")
    endif ()

    _pedigree_musl_sdk_paths("${ARG_ARCHITECTURE}" "${ARG_LAYOUT}")

    _pedigree_musl_sdk_require_regular_file(
        "${ARG_ROOT}"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libc.so"
        "runtime file")

    set(sdk_libraries
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libc.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libcrypt.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libdl.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libm.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libpthread.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libresolv.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/librt.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libutil.a"
        "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libxnet.a")
    foreach(relative IN LISTS sdk_libraries)
        _pedigree_musl_sdk_require_regular_file(
            "${ARG_ROOT}" "${relative}" "SDK library")
    endforeach()

    foreach(filename crt1.o rcrt1.o Scrt1.o crti.o crtn.o)
        _pedigree_musl_sdk_require_regular_file(
            "${ARG_ROOT}"
            "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/${filename}"
            "CRT object")
    endforeach()

    foreach(filename
        stdio.h
        stdlib.h
        stdint.h
        unistd.h
        bits/syscall.h
        sys/syscall.h)
        _pedigree_musl_sdk_require_regular_file(
            "${ARG_ROOT}"
            "${PEDIGREE_MUSL_SDK_INCLUDE_DIR}/${filename}"
            "header")
    endforeach()

    set(loader "${ARG_ROOT}/${PEDIGREE_MUSL_SDK_LOADER}")
    if (NOT IS_SYMLINK "${loader}")
        message(FATAL_ERROR
            "musl SDK dynamic loader must be a symlink: ${PEDIGREE_MUSL_SDK_LOADER}")
    endif ()
    file(READ_SYMLINK "${loader}" loader_target)
    if (IS_ABSOLUTE "${loader_target}")
        message(FATAL_ERROR
            "musl SDK dynamic loader target must be relative: ${loader_target}")
    endif ()
    if (NOT EXISTS "${loader}")
        message(FATAL_ERROR
            "musl SDK dynamic loader symlink is dangling: ${loader_target}")
    endif ()

    get_filename_component(loader_directory "${loader}" DIRECTORY)
    file(REAL_PATH "${loader_directory}/${loader_target}" resolved_loader)
    file(REAL_PATH
        "${ARG_ROOT}/${PEDIGREE_MUSL_SDK_LIBRARY_DIR}/libc.so"
        expected_loader)
    if (NOT "${resolved_loader}" STREQUAL "${expected_loader}")
        message(FATAL_ERROR
            "musl SDK dynamic loader must resolve to in-root libc.so: ${loader_target}")
    endif ()

    if (ARG_LOADER_TARGET_OUTPUT)
        set(${ARG_LOADER_TARGET_OUTPUT} "${loader_target}" PARENT_SCOPE)
    endif ()
endfunction()

function(_pedigree_musl_sdk_json_get output json)
    string(JSON value ERROR_VARIABLE error GET "${json}" ${ARGN})
    if (NOT "${error}" STREQUAL "NOTFOUND")
        list(JOIN ARGN "." field)
        message(FATAL_ERROR
            "invalid musl SDK manifest field ${field}: ${error}")
    endif ()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_pedigree_musl_sdk_json_type output json)
    string(JSON value ERROR_VARIABLE error TYPE "${json}" ${ARGN})
    if (NOT "${error}" STREQUAL "NOTFOUND")
        list(JOIN ARGN "." field)
        message(FATAL_ERROR
            "invalid musl SDK manifest field ${field}: ${error}")
    endif ()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_pedigree_musl_sdk_json_string output json)
    _pedigree_musl_sdk_json_type(type "${json}" ${ARGN})
    if (NOT "${type}" STREQUAL "STRING")
        list(JOIN ARGN "." field)
        message(FATAL_ERROR
            "musl SDK manifest field ${field} must be a string")
    endif ()
    _pedigree_musl_sdk_json_get(value "${json}" ${ARGN})
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_pedigree_musl_sdk_json_require_members json)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "PATH;MEMBERS")
    string(JSON type ERROR_VARIABLE error TYPE "${json}" ${ARG_PATH})
    if (NOT "${error}" STREQUAL "NOTFOUND" OR NOT "${type}" STREQUAL "OBJECT")
        list(JOIN ARG_PATH "." field)
        if ("${field}" STREQUAL "")
            set(field "manifest")
        endif ()
        message(FATAL_ERROR
            "musl SDK manifest field ${field} must be an object")
    endif ()

    string(JSON length LENGTH "${json}" ${ARG_PATH})
    list(LENGTH ARG_MEMBERS expected_length)
    if (NOT length EQUAL expected_length)
        list(JOIN ARG_PATH "." field)
        if ("${field}" STREQUAL "")
            set(field "manifest")
        endif ()
        message(FATAL_ERROR
            "musl SDK manifest field ${field} has unexpected members")
    endif ()

    math(EXPR last_member "${length} - 1")
    foreach(index RANGE 0 ${last_member})
        string(JSON member MEMBER "${json}" ${ARG_PATH} ${index})
        if (NOT "${member}" IN_LIST ARG_MEMBERS)
            list(JOIN ARG_PATH "." field)
            if ("${field}" STREQUAL "")
                set(field "manifest")
            endif ()
            message(FATAL_ERROR
                "musl SDK manifest field ${field} has unexpected member ${member}")
        endif ()
    endforeach()
endfunction()

function(pedigree_validate_musl_sdk_manifest)
    cmake_parse_arguments(PARSE_ARGV 0 ARG ""
        "MANIFEST;ROOT;EXPECTED_BUILD_ID" "")
    if (NOT DEFINED ARG_MANIFEST OR NOT IS_ABSOLUTE "${ARG_MANIFEST}" OR
        NOT EXISTS "${ARG_MANIFEST}" OR IS_DIRECTORY "${ARG_MANIFEST}")
        message(FATAL_ERROR
            "musl SDK manifest must be an existing absolute file: ${ARG_MANIFEST}")
    endif ()
    if (NOT DEFINED ARG_ROOT OR "${ARG_ROOT}" STREQUAL "")
        message(FATAL_ERROR
            "pedigree_validate_musl_sdk_manifest requires ROOT")
    endif ()

    file(READ "${ARG_MANIFEST}" manifest)
    _pedigree_musl_sdk_json_require_members("${manifest}"
        MEMBERS schema schema_version component target layout provenance)
    _pedigree_musl_sdk_json_require_members("${manifest}"
        PATH component
        MEMBERS name version port_revision)
    _pedigree_musl_sdk_json_require_members("${manifest}"
        PATH target
        MEMBERS architecture abi profile page_size dt_relr)
    _pedigree_musl_sdk_json_require_members("${manifest}"
        PATH layout
        MEMBERS
            name
            include_directory
            library_directory
            dynamic_loader
            dynamic_loader_target)
    _pedigree_musl_sdk_json_require_members("${manifest}"
        PATH provenance
        MEMBERS
            upstream_sha256
            pedigree_revision
            build_id
            compiler_target
            compiler_version)

    _pedigree_musl_sdk_json_string(schema "${manifest}" schema)
    _pedigree_musl_sdk_json_get(schema_version "${manifest}" schema_version)
    _pedigree_musl_sdk_json_type(
        schema_version_type "${manifest}" schema_version)
    if (NOT "${schema}" STREQUAL "${PEDIGREE_MUSL_SDK_MANIFEST_SCHEMA}" OR
        NOT "${schema_version_type}" STREQUAL "NUMBER" OR
        NOT "${schema_version}" STREQUAL
            "${PEDIGREE_MUSL_SDK_MANIFEST_SCHEMA_VERSION}")
        message(FATAL_ERROR
            "unsupported musl SDK manifest schema: ${schema} ${schema_version}")
    endif ()

    _pedigree_musl_sdk_json_string(name "${manifest}" component name)
    _pedigree_musl_sdk_json_string(version "${manifest}" component version)
    _pedigree_musl_sdk_json_get(
        port_revision "${manifest}" component port_revision)
    _pedigree_musl_sdk_json_type(
        port_revision_type "${manifest}" component port_revision)
    if (NOT "${name}" STREQUAL "musl")
        message(FATAL_ERROR "musl SDK component name must be musl: ${name}")
    endif ()
    _pedigree_musl_sdk_require_token("component.version" "${version}")
    if (NOT "${port_revision_type}" STREQUAL "NUMBER" OR
        NOT "${port_revision}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "component.port_revision must be a positive integer: ${port_revision}")
    endif ()

    _pedigree_musl_sdk_json_string(
        architecture "${manifest}" target architecture)
    _pedigree_musl_sdk_json_string(abi "${manifest}" target abi)
    _pedigree_musl_sdk_json_string(profile "${manifest}" target profile)
    _pedigree_musl_sdk_json_get(page_size "${manifest}" target page_size)
    _pedigree_musl_sdk_json_type(
        page_size_type "${manifest}" target page_size)
    _pedigree_musl_sdk_json_get(dt_relr "${manifest}" target dt_relr)
    _pedigree_musl_sdk_json_type(dt_relr_type "${manifest}" target dt_relr)
    _pedigree_musl_sdk_require_token("target.abi" "${abi}")
    if (NOT "${profile}" STREQUAL "target" AND
        NOT "${profile}" STREQUAL "hosted")
        message(FATAL_ERROR
            "target.profile must be target or hosted: ${profile}")
    endif ()
    if (NOT "${page_size_type}" STREQUAL "NUMBER" OR
        NOT "${page_size}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "target.page_size must be a positive integer: ${page_size}")
    endif ()
    math(EXPR page_size_mask "${page_size} - 1")
    math(EXPR page_size_remainder "${page_size} & ${page_size_mask}")
    if (NOT page_size_remainder EQUAL 0)
        message(FATAL_ERROR
            "target.page_size must be a power of two: ${page_size}")
    endif ()
    if (NOT "${dt_relr_type}" STREQUAL "BOOLEAN")
        message(FATAL_ERROR "target.dt_relr must be a boolean")
    endif ()

    _pedigree_musl_sdk_json_string(layout "${manifest}" layout name)
    _pedigree_musl_sdk_json_string(
        include_dir "${manifest}" layout include_directory)
    _pedigree_musl_sdk_json_string(
        library_dir "${manifest}" layout library_directory)
    _pedigree_musl_sdk_json_string(
        dynamic_loader "${manifest}" layout dynamic_loader)
    _pedigree_musl_sdk_json_string(
        manifest_loader_target "${manifest}" layout dynamic_loader_target)
    _pedigree_musl_sdk_paths("${architecture}" "${layout}")
    if (NOT "${include_dir}" STREQUAL "${PEDIGREE_MUSL_SDK_INCLUDE_DIR}" OR
        NOT "${library_dir}" STREQUAL "${PEDIGREE_MUSL_SDK_LIBRARY_DIR}" OR
        NOT "${dynamic_loader}" STREQUAL "${PEDIGREE_MUSL_SDK_LOADER}")
        message(FATAL_ERROR "musl SDK manifest layout paths are inconsistent")
    endif ()

    _pedigree_musl_sdk_json_string(
        upstream_sha256 "${manifest}" provenance upstream_sha256)
    _pedigree_musl_sdk_json_string(
        pedigree_revision "${manifest}" provenance pedigree_revision)
    _pedigree_musl_sdk_json_string(
        build_id "${manifest}" provenance build_id)
    _pedigree_musl_sdk_json_string(
        compiler_target "${manifest}" provenance compiler_target)
    _pedigree_musl_sdk_json_string(
        compiler_version "${manifest}" provenance compiler_version)
    string(LENGTH "${upstream_sha256}" upstream_sha256_length)
    if (NOT upstream_sha256_length EQUAL 64 OR
        NOT "${upstream_sha256}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "provenance.upstream_sha256 must be 64 lowercase hexadecimal characters")
    endif ()
    _pedigree_musl_sdk_require_token(
        "provenance.pedigree_revision" "${pedigree_revision}")
    _pedigree_musl_sdk_require_token("provenance.build_id" "${build_id}")
    if (ARG_EXPECTED_BUILD_ID AND
        NOT "${build_id}" STREQUAL "${ARG_EXPECTED_BUILD_ID}")
        message(FATAL_ERROR
            "musl SDK build ID does not match the requested derivation: "
            "${build_id} != ${ARG_EXPECTED_BUILD_ID}")
    endif ()
    _pedigree_musl_sdk_require_token(
        "provenance.compiler_target" "${compiler_target}")
    _pedigree_musl_sdk_require_token(
        "provenance.compiler_version" "${compiler_version}")

    pedigree_validate_musl_sdk_root(
        ROOT "${ARG_ROOT}"
        ARCHITECTURE "${architecture}"
        LAYOUT "${layout}"
        LOADER_TARGET_OUTPUT actual_loader_target)
    if (NOT "${manifest_loader_target}" STREQUAL "${actual_loader_target}")
        message(FATAL_ERROR
            "manifest loader target does not match the SDK root: "
            "${manifest_loader_target} != ${actual_loader_target}")
    endif ()
endfunction()
