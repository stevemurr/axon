# Download the Microsoft.Web.WebView2 SDK (Windows GUI backend, Phase 3).
# A .nupkg is just a zip; nuget.org's flat-container URL is stable and
# version-addressed, and DOWNLOAD_NAME forces a .zip filename so CMake's
# archive extraction recognizes it.
#
# We deliberately link the STATIC loader (build/native/x64/
# WebView2LoaderStatic.lib) rather than WebView2Loader.dll: hosts resolve a
# plugin's dependent DLLs only because they LoadLibraryEx the .clap with
# LOAD_WITH_ALTERED_SEARCH_PATH, and shipping one less DLL removes a whole
# packaging failure mode (package_windows.py stays unchanged).
#
# Updating:
#   - bump WEBVIEW2_VERSION below
#   - sha256sum -b microsoft.web.webview2.<ver>.nupkg  (from the URL pattern
#     below) and paste into WEBVIEW2_SHA256
include(FetchContent)

set(WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "Microsoft.Web.WebView2 SDK version")

# Known-good hash, verified against the nuget.org flat-container asset.
# NEVER leave empty (same integrity rule as FetchOnnxRuntime.cmake).
set(WEBVIEW2_SHA256
    "ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74"
    CACHE STRING "sha256 of the WebView2 SDK nupkg")

set(WEBVIEW2_URL
    "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg"
)

if(WEBVIEW2_SHA256)
    FetchContent_Declare(webview2_sdk
        URL ${WEBVIEW2_URL}
        URL_HASH SHA256=${WEBVIEW2_SHA256}
        DOWNLOAD_NAME webview2_sdk.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
    FetchContent_Declare(webview2_sdk
        URL ${WEBVIEW2_URL}
        DOWNLOAD_NAME webview2_sdk.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_MakeAvailable(webview2_sdk)

set(WEBVIEW2_ROOT "${webview2_sdk_SOURCE_DIR}")
set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_ROOT}/build/native/include"
    CACHE PATH "" FORCE)
set(WEBVIEW2_LOADER_STATIC "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib"
    CACHE FILEPATH "" FORCE)

if(NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
    message(FATAL_ERROR "FetchWebView2: WebView2.h not found under ${WEBVIEW2_INCLUDE_DIR} — SDK layout changed?")
endif()
if(NOT EXISTS "${WEBVIEW2_LOADER_STATIC}")
    message(FATAL_ERROR "FetchWebView2: WebView2LoaderStatic.lib not found at ${WEBVIEW2_LOADER_STATIC} — SDK layout changed?")
endif()
