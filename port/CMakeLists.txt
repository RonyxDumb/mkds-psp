cmake_minimum_required(VERSION 3.22)
project(mkds_psp C)

# Two targets from the same sources:
#  - PSP (configure with psp-cmake): the EBOOT.PBP;
#  - Linux test build (32-bit x86): runs headless and writes both screens as PNG images, for debugging.
# The game assumes 32-bit pointers (structures and files loaded in place): both are 32-bit.

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(DECOMP ${REPO}/decomp)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(DECOMP_VERSION eur)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ------------------------------------------------------------------ decomp sources, prepared for the port
file(GLOB_RECURSE DECOMP_INPUTS CONFIGURE_DEPENDS ${DECOMP}/src/*.c ${DECOMP}/libs/*.c ${DECOMP}/include/types.h
     ${DECOMP}/config/${DECOMP_VERSION}/arm9/overlays/*/delinks.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${REPO}/tools/portify.py ${CMAKE_CURRENT_SOURCE_DIR}/exclude.txt
    ${DECOMP_INPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/registers.h)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${REPO}/tools/portify.py ${DECOMP} ${GEN} ${CMAKE_CURRENT_SOURCE_DIR}/exclude.txt ${DECOMP_VERSION}
    RESULT_VARIABLE PORTIFY_RESULT)
if (NOT PORTIFY_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/portify.py failed")
endif()
file(STRINGS ${GEN}/sources.txt GAME_SOURCES)

if (PSP)
    set(OPT -O2 -G0)
else()
    set(OPT -O2)
endif()

add_library(game STATIC ${GAME_SOURCES})
target_include_directories(game PRIVATE
    ${GEN}/include
    ${GEN}/msl_include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    )
target_compile_definitions(game PRIVATE version_EUR)
target_compile_options(game PRIVATE
    -std=gnu11
    -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prefix.h
    ${OPT}
    -fno-strict-aliasing   # the game type-puns freely
    -fwrapv                # and relies on wrapping arithmetic
    -fcommon               # tentative definitions shared between files, as CodeWarrior allows
    -fshort-wchar          # wchar_t is 16 bits on the DS
    -Dconst=               # constant data is RAM on the DS and the game patches some of it: keep it writable
    -fno-zero-initialized-in-bss # keep data in definition order: some tables run on into the next definition
    -ffunction-sections
    -w)
# Plain char: unsigned in game code, signed in the libraries (tools/project.py).
foreach(SRC ${GAME_SOURCES})
    if (SRC MATCHES "/src/libs/")
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS -fsigned-char)
    else()
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS -funsigned-char)
    endif()
endforeach()

# ------------------------------------------------------------------ platform layer
set(P ${CMAKE_CURRENT_SOURCE_DIR}/platform)
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS ${P}/*.c)
set(PLATFORM_SOURCES ${PLATFORM_SOURCES} ${P}/hostside/memory.c ${P}/hostside/direct.c)

function(port_target T)
    target_include_directories(${T} PRIVATE ${GEN}/include ${P}/include ${P} ${CMAKE_CURRENT_SOURCE_DIR}/psp)
    target_compile_definitions(${T} PRIVATE version_EUR)
    target_compile_options(${T} PRIVATE ${OPT} -fno-strict-aliasing -fwrapv -Wall -Wno-unused-function
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
endfunction()

if (PSP)
    file(GLOB PSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/psp/*.c)
    add_executable(mkds ${PLATFORM_SOURCES} ${PSP_SOURCES})
    port_target(mkds)
    target_link_libraries(mkds PRIVATE -Wl,--whole-archive game -Wl,--no-whole-archive
        pspgu pspgum pspdisplay pspge pspctrl pspaudio psppower psprtc pspdebug m)
    create_pbp_file(TARGET mkds
        ICON_PATH NULL
        BACKGROUND_PATH NULL
        PREVIEW_PATH NULL
        TITLE "Mario Kart DS"
        VERSION 01.00
        BUILD_PRX)
else()
    # Headless test build for Linux (i386).
    file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host/*.c)
    add_executable(mkds ${PLATFORM_SOURCES} ${P}/hostside/hostapi_impl.c ${P}/hostside/online.c ${HOST_SOURCES})
    port_target(mkds)
    target_link_libraries(mkds PRIVATE -Wl,--whole-archive game -Wl,--no-whole-archive pthread m dl)
    target_link_options(mkds PRIVATE -Wl,-z,now -Wl,-z,norelro)
endif()
