cmake_minimum_required(VERSION 3.22)

project(mod_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MOD_ID "mod_host" CACHE STRING "LeviLauncher mod package directory name")
set(MOD_NAME "Mod Host" CACHE STRING "LeviLauncher mod display name")
set(MOD_AUTHOR "you" CACHE STRING "LeviLauncher mod author")
set(MOD_VERSION "0.1.0" CACHE STRING "LeviLauncher mod version")
set(MOD_LIBRARY_NAME "mod_host" CACHE STRING "Native library name without lib prefix or .so suffix")
set(MOD_MINECRAFT_VERSIONS "[\"1.26.62.1\", \"1.26.62\"]" CACHE STRING "Raw JSON array for manifest minecraft_versions")
set(MOD_ICON "" CACHE STRING "Optional package-relative icon path, such as icon.png")

include(FetchContent)

FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2
    SOURCE_SUBDIR cmake/preloader-sdk
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_Declare(
    boost_pfr
    GIT_REPOSITORY https://github.com/boostorg/pfr.git
    GIT_TAG boost-1.88.0
)
FetchContent_Declare(
    magic_enum
    GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
    GIT_TAG v0.9.7
)
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.6.tar.gz
)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0
)
set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)
FetchContent_MakeAvailable(preloader_android nlohmann_json boost_pfr magic_enum fmt lua)

# The launcher's runtime reads an extra ModuleInfo member (onKeybind) that the
# pinned SDK header does not have. Without this patch registerModule() reads
# past the end of the struct and crashes the game.
find_program(LEVI_PYTHON NAMES python3 python)
if(NOT LEVI_PYTHON)
    message(FATAL_ERROR "python3 is required to patch the preloader SDK header")
endif()
execute_process(
    COMMAND "${LEVI_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_sdk.py" "${preloader_android_SOURCE_DIR}"
    RESULT_VARIABLE LEVI_SDK_PATCH_RESULT
)
if(NOT LEVI_SDK_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Patching the preloader SDK header failed (see output above)")
endif()

FetchContent_GetProperties(preloader_android)
set(LEVI_PRELOADER_INCLUDE_DIR "${preloader_android_SOURCE_DIR}/include")

if(NOT ANDROID)
    set(LEVI_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-config")

    add_executable(levi_config_generator
        src/config_generator.cpp
        src/mod/Config.cpp
    )

    target_include_directories(levi_config_generator
        PRIVATE
            src
            "${LEVI_PRELOADER_INCLUDE_DIR}"
    )

    target_compile_definitions(levi_config_generator PRIVATE PL_CONFIG_NO_RUNTIME)
    target_link_libraries(levi_config_generator PRIVATE
        nlohmann_json::nlohmann_json
        Boost::pfr
        magic_enum::magic_enum
    )

    add_custom_target(levi_generate_config
        COMMAND levi_config_generator "${LEVI_GENERATED_CONFIG_DIR}"
        DEPENDS levi_config_generator
        COMMENT "Generating config.json and config.schema.json"
        VERBATIM
    )

    return()
endif()

if(NOT DEFINED LEVI_PACKAGE_CONFIG_DIR)
    set(LEVI_PACKAGE_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config" CACHE PATH
        "Directory containing config.json and config.schema.json to package")
endif()

get_filename_component(LEVI_PACKAGE_CONFIG_DIR "${LEVI_PACKAGE_CONFIG_DIR}" ABSOLUTE)
set(LEVI_PACKAGE_CONFIG_JSON "${LEVI_PACKAGE_CONFIG_DIR}/config.json")
set(LEVI_PACKAGE_CONFIG_SCHEMA "${LEVI_PACKAGE_CONFIG_DIR}/config.schema.json")

if(NOT IS_DIRECTORY "${LEVI_PACKAGE_CONFIG_DIR}")
    message(FATAL_ERROR "LEVI_PACKAGE_CONFIG_DIR must be a directory: ${LEVI_PACKAGE_CONFIG_DIR}")
endif()

if(NOT EXISTS "${LEVI_PACKAGE_CONFIG_JSON}")
    message(FATAL_ERROR "LEVI_PACKAGE_CONFIG_DIR must contain config.json: ${LEVI_PACKAGE_CONFIG_JSON}")
endif()

if(NOT EXISTS "${LEVI_PACKAGE_CONFIG_SCHEMA}")
    message(FATAL_ERROR "LEVI_PACKAGE_CONFIG_DIR must contain config.schema.json: ${LEVI_PACKAGE_CONFIG_SCHEMA}")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" AND
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "Unsupported Android ABI: ${CMAKE_ANDROID_ARCH_ABI}")
endif()

option(LEVI_LINK_PRELOADER
       "Build and link the preloader target instead of leaving SDK symbols for runtime lookup"
       ON)

if(LEVI_LINK_PRELOADER AND NOT TARGET preloader)
    add_subdirectory("${preloader_android_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

# Lua is compiled as C++ so that script errors unwind like C++ exceptions and
# destructors in the host code run correctly.
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua_core STATIC ${LUA_SOURCES})
target_include_directories(lua_core PUBLIC "${lua_SOURCE_DIR}/src")
set_target_properties(lua_core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(lua_core PRIVATE -w)

add_library(${MOD_LIBRARY_NAME} SHARED
    src/main.cpp
    src/mod/Config.cpp
    src/mod/JavaBridge.cpp
    src/mod/LuaHost.cpp
    src/mod/MyMod.cpp
)

set_target_properties(${MOD_LIBRARY_NAME} PROPERTIES
    OUTPUT_NAME "${MOD_LIBRARY_NAME}"
)

target_include_directories(${MOD_LIBRARY_NAME}
    PRIVATE
        src
        "${LEVI_PRELOADER_INCLUDE_DIR}"
)

target_compile_options(${MOD_LIBRARY_NAME}
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden -fvisibility-inlines-hidden>
)

target_link_options(${MOD_LIBRARY_NAME}
    PRIVATE
        "-Wl,-z,max-page-size=16384"
)

find_library(log-lib log REQUIRED)
target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE
    ${log-lib}
    ${CMAKE_DL_LIBS}
    lua_core
    nlohmann_json::nlohmann_json
    Boost::pfr
    magic_enum::magic_enum
    fmt::fmt
)

if(TARGET preloader)
    target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE preloader)
else()
    target_link_options(${MOD_LIBRARY_NAME} PRIVATE
        "-Wl,--unresolved-symbols=ignore-all")
endif()

if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "CMAKE_STRIP is required to strip the generated library.")
endif()

add_custom_command(TARGET ${MOD_LIBRARY_NAME} POST_BUILD
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
    COMMENT "Stripping symbols from $<TARGET_FILE_NAME:${MOD_LIBRARY_NAME}>"
    VERBATIM
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/manifest.json"
    @ONLY
)

set(LEVI_PACKAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/package")
set(LEVI_PACKAGE_MOD_DIR "${LEVI_PACKAGE_ROOT}/${MOD_ID}")
set(LEVI_PACKAGE_FILE "${CMAKE_CURRENT_BINARY_DIR}/${MOD_ID}-${MOD_VERSION}-${CMAKE_ANDROID_ARCH_ABI}.levipack")
set(LEVI_PACKAGE_ICON_COMMAND)
set(LEVI_PACKAGE_CONFIG_COMMAND
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${LEVI_PACKAGE_CONFIG_DIR}" "${LEVI_PACKAGE_MOD_DIR}/config")

if(NOT MOD_ICON STREQUAL "")
    if(IS_ABSOLUTE "${MOD_ICON}")
        message(FATAL_ERROR "MOD_ICON must be a package-relative path")
    endif()

    if(MOD_ICON MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "MOD_ICON must not contain ..")
    endif()

    set(MOD_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_ICON}")
    if(NOT EXISTS "${MOD_ICON_SOURCE}")
        message(FATAL_ERROR "MOD_ICON does not exist: ${MOD_ICON_SOURCE}")
    endif()

    get_filename_component(MOD_ICON_DIR "${MOD_ICON}" DIRECTORY)
    if(NOT MOD_ICON_DIR STREQUAL "")
        list(APPEND LEVI_PACKAGE_ICON_COMMAND
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEVI_PACKAGE_MOD_DIR}/${MOD_ICON_DIR}")
    endif()
    list(APPEND LEVI_PACKAGE_ICON_COMMAND
        COMMAND "${CMAKE_COMMAND}" -E copy "${MOD_ICON_SOURCE}" "${LEVI_PACKAGE_MOD_DIR}/${MOD_ICON}")
endif()

add_custom_target(levi_package
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${LEVI_PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEVI_PACKAGE_MOD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${MOD_LIBRARY_NAME}>" "${LEVI_PACKAGE_MOD_DIR}/lib${MOD_LIBRARY_NAME}.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" "${LEVI_PACKAGE_MOD_DIR}/manifest.json"
    ${LEVI_PACKAGE_ICON_COMMAND}
    ${LEVI_PACKAGE_CONFIG_COMMAND}
    COMMAND "${CMAKE_COMMAND}" -E chdir "${LEVI_PACKAGE_ROOT}" "${CMAKE_COMMAND}" -E tar "cf" "${LEVI_PACKAGE_FILE}" --format=zip "${MOD_ID}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS ${MOD_LIBRARY_NAME}
    COMMENT "Packaging ${MOD_ID} for LeviLauncher"
    VERBATIM
)
