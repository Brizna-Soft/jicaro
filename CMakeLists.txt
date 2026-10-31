cmake_minimum_required(VERSION 3.25)

# --- vcpkg: triplet propio (todo estático, solo Release) ---
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")
set(VCPKG_TARGET_TRIPLET "x64-windows-static-rel" CACHE STRING "")
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _vcpkg)
    else()
        set(_vcpkg "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vcpkg")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${_vcpkg}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# CRT estático, igual que las bibliotecas de vcpkg. Solo Release: el triplet no compila versiones Debug.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)

file(STRINGS version.txt APP_VERSION LIMIT_COUNT 1)
project(Jicaro VERSION ${APP_VERSION} LANGUAGES C CXX RC)
if(NOT APP_VERSION_TEXTO)
    set(APP_VERSION_TEXTO "${APP_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wxWidgets CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generado/version.h" @ONLY)

# /external: sin avisos de los encabezados de las bibliotecas (los que se incluyen con <>)
set(OPCIONES_CXX /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus /guard:cf /external:anglebrackets /external:W0)
# WIN32_LEAN_AND_MEAN: windows.h sin winsock.h (choca con el winsock2.h que usa wx)
set(DEFINICIONES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

# --- Núcleo sin ventanas: libarchive, rutas, partes y marca de la Web. Lo usan el programa y las pruebas. ---
file(GLOB NUCLEO CONFIGURE_DEPENDS src/nucleo/*.cpp src/nucleo/*.hpp)
add_library(jicaro-nucleo STATIC ${NUCLEO})
target_include_directories(jicaro-nucleo PUBLIC src/nucleo)
target_compile_definitions(jicaro-nucleo PUBLIC ${DEFINICIONES})
target_compile_options(jicaro-nucleo PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OPCIONES_CXX}>)
target_link_libraries(jicaro-nucleo PUBLIC LibArchive::LibArchive bcrypt ole32 oleaut32 uuid urlmon shell32 shlwapi)

# --- El programa ---
file(GLOB FUENTES CONFIGURE_DEPENDS src/*.cpp src/*.hpp)
add_executable(Jicaro WIN32 ${FUENTES} res/app.rc)
target_include_directories(Jicaro PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generado")
target_compile_options(Jicaro PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OPCIONES_CXX}>)
target_link_options(Jicaro PRIVATE /MANIFEST:NO /guard:cf)
target_link_libraries(Jicaro PRIVATE jicaro-nucleo wx::core wx::base nlohmann_json::nlohmann_json winhttp)

# --- Pruebas sin ventana (no se publican) ---
# JICARO_PRUEBAS: la Papelera puede ser una carpeta de prueba (solo aquí; el programa no lo trae)
add_library(jicaro-nucleo-pruebas STATIC ${NUCLEO})
target_include_directories(jicaro-nucleo-pruebas PUBLIC src/nucleo)
target_compile_definitions(jicaro-nucleo-pruebas PUBLIC ${DEFINICIONES} JICARO_PRUEBAS)
target_compile_options(jicaro-nucleo-pruebas PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OPCIONES_CXX}>)
target_link_libraries(jicaro-nucleo-pruebas PUBLIC LibArchive::LibArchive bcrypt ole32 oleaut32 uuid urlmon shell32 shlwapi)
foreach(prueba prueba-rutas prueba-nucleo)
    add_executable(${prueba} pruebas/${prueba}.cpp)
    target_compile_options(${prueba} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OPCIONES_CXX}>)
    target_link_libraries(${prueba} PRIVATE jicaro-nucleo-pruebas)
endforeach()
