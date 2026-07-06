# Download the prebuilt ONNX Runtime release tarball for the current platform.
# These aren't on a package registry, so we grab the tgz from the GitHub
# release. One archive + SHA pin per supported platform, same ORT_VERSION.
#
# Updating:
#   - bump ORT_VERSION below
#   - for each platform archive: sha256sum -b onnxruntime-<plat>-X.Y.Z.tgz
#   - paste into the matching ORT_SHA256_* below
include(FetchContent)

set(ORT_VERSION "1.20.1" CACHE STRING "onnxruntime release tag")

# Known-good hashes (verified against the GitHub release assets). Replace on
# version bump. NEVER leave these empty: an empty hash means any cache-clean
# rebuild downloads and links the tarball with zero integrity verification.
set(ORT_SHA256_OSX_ARM64  "b678fc3c2354c771fea4fba420edeccfba205140088334df801e7fc40e83a57a")
set(ORT_SHA256_LINUX_X64  "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105")

if(APPLE)
    set(ORT_PLATFORM "osx-arm64")
    set(_ort_sha_default "${ORT_SHA256_OSX_ARM64}")
elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(ORT_PLATFORM "linux-x64")
    set(_ort_sha_default "${ORT_SHA256_LINUX_X64}")
else()
    message(FATAL_ERROR
        "FetchOnnxRuntime: no ORT prebuilt configured for this platform "
        "(system=${CMAKE_SYSTEM_NAME}, processor=${CMAKE_SYSTEM_PROCESSOR}). "
        "Add an archive name + SHA pin above.")
endif()

set(ORT_ARCHIVE "onnxruntime-${ORT_PLATFORM}-${ORT_VERSION}")
set(ORT_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ARCHIVE}.tgz"
)
set(ORT_SHA256 "${_ort_sha_default}"
    CACHE STRING "sha256 of the ORT tarball; empty = trust the URL")

if(ORT_SHA256)
    FetchContent_Declare(onnxruntime URL ${ORT_URL} URL_HASH SHA256=${ORT_SHA256})
else()
    FetchContent_Declare(onnxruntime URL ${ORT_URL})
endif()

FetchContent_MakeAvailable(onnxruntime)

set(ONNXRUNTIME_ROOT "${onnxruntime_SOURCE_DIR}")
set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include" CACHE PATH "" FORCE)

if(APPLE)
    # The release tgz ships libonnxruntime.<version>.dylib and a versionless
    # symlink libonnxruntime.dylib. We link against the symlink and copy the
    # real dylib into the .clap bundle's Frameworks/.
    set(ONNXRUNTIME_LIBRARY "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.dylib" CACHE FILEPATH "" FORCE)
    set(ONNXRUNTIME_DYLIB_REAL "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.${ORT_VERSION}.dylib" CACHE FILEPATH "" FORCE)
else()
    # Linux tgz ships libonnxruntime.so.<version> plus .so / .so.1 symlinks
    # (SONAME libonnxruntime.so.1). Link the versionless symlink; packaging
    # copies the real file + SONAME symlink next to the plugin ($ORIGIN rpath).
    set(ONNXRUNTIME_LIBRARY "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so" CACHE FILEPATH "" FORCE)
    set(ONNXRUNTIME_DYLIB_REAL "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so.${ORT_VERSION}" CACHE FILEPATH "" FORCE)
endif()
