# Locate the Slang shader compiler (slangc). Prefer one on PATH or under
# SAFFRON_SLANG_DIR; otherwise fetch the official prebuilt release.
set(SAFFRON_SLANG_VERSION "2026.10")

find_program(SAFFRON_SLANGC
    NAMES slangc
    HINTS "${SAFFRON_SLANG_DIR}/bin" ENV SAFFRON_SLANG_DIR
    DOC "Slang shader compiler (slangc)")

if(NOT SAFFRON_SLANGC)
    message(STATUS "slangc not found locally; fetching Slang ${SAFFRON_SLANG_VERSION} prebuilt")
    include(FetchContent)
    FetchContent_Declare(slang_prebuilt
        URL https://github.com/shader-slang/slang/releases/download/v${SAFFRON_SLANG_VERSION}/slang-${SAFFRON_SLANG_VERSION}-linux-x86_64.tar.gz
        URL_HASH SHA256=a0a39926398dc1333d0f3d29addeffa92739735bd042b91faf5052f504c23864)
    FetchContent_MakeAvailable(slang_prebuilt)
    set(SAFFRON_SLANGC "${slang_prebuilt_SOURCE_DIR}/bin/slangc" CACHE FILEPATH "Slang compiler" FORCE)
endif()

message(STATUS "Using slangc: ${SAFFRON_SLANGC}")
