# The target PUP and host protoc are kept separate because protoc is a build
# tool. The generated C++ API used by this protocol is compatible across this
# adjacent release pair, but protobuf's generated header intentionally rejects
# the combination. Keep the bypass explicit and limited to that guard.

if (NOT DEFINED HEADER OR NOT EXISTS "${HEADER}")
    message(FATAL_ERROR "Generated protobuf header was not found: ${HEADER}")
endif ()

file(READ "${HEADER}" contents)
string(REGEX REPLACE
    "#if PROTOBUF_VERSION != [0-9]+"
    "#if 0"
    updated "${contents}")

if (updated STREQUAL contents)
    message(FATAL_ERROR "Generated protobuf version guard was not found in ${HEADER}")
endif ()

file(WRITE "${HEADER}" "${updated}")
