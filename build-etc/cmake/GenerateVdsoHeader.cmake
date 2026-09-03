if (NOT DEFINED INPUT OR NOT EXISTS "${INPUT}" OR IS_DIRECTORY "${INPUT}")
    message(FATAL_ERROR "INPUT must name the built vDSO: ${INPUT}")
endif ()
if (NOT DEFINED OUTPUT OR NOT IS_ABSOLUTE "${OUTPUT}")
    message(FATAL_ERROR "OUTPUT must be an absolute path: ${OUTPUT}")
endif ()
if (NOT DEFINED PAGE_SIZE OR NOT PAGE_SIZE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "PAGE_SIZE must be a positive integer: ${PAGE_SIZE}")
endif ()

file(READ "${INPUT}" VDSO_HEX HEX)
file(SIZE "${INPUT}" VDSO_LENGTH)
math(EXPR VDSO_PAGES "(${VDSO_LENGTH} + ${PAGE_SIZE} - 1) / ${PAGE_SIZE}")
string(REGEX REPLACE "(..)" "0x\\1," VDSO_BYTES "${VDSO_HEX}")

file(WRITE "${OUTPUT}"
    "const unsigned char __vdso_so[] = {${VDSO_BYTES}};\n"
    "const int __vdso_so_len = ${VDSO_LENGTH};\n"
    "const int __vdso_so_pages = ${VDSO_PAGES};\n")
