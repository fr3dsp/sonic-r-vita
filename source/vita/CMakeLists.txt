cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(sonicr_vita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Sonic R")
set(VITA_TITLEID  "SONICR001")
set(VITA_VERSION  "01.00")

set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

file(GLOB_RECURSE SONICR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

set(SONICR_VITA_LINK_PAD 8192 CACHE STRING "Bytes of rodata padding added so vita-elf-create has room for SCE data")

set(SONICR_LINK_PAD_FILE "${CMAKE_CURRENT_BINARY_DIR}/vita_link_pad.c")
file(GENERATE OUTPUT "${SONICR_LINK_PAD_FILE}"
  CONTENT "const unsigned char sonicr_vita_link_pad[${SONICR_VITA_LINK_PAD}] __attribute__((used)) = { 1 };\n")

add_executable(${PROJECT_NAME} ${SONICR_SOURCES} "${SONICR_LINK_PAD_FILE}")

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(${PROJECT_NAME} PRIVATE
  SONICR_SDL
  SONICR_VITA
  SONICR_MATCHMAKER
  _GNU_SOURCE
)

target_compile_options(${PROJECT_NAME} PRIVATE
  -std=gnu11
  -O2
  -g
  -fno-common
  -fno-strict-aliasing
  -Wall
  -Wno-unused-variable
  -Wno-unused-function
  -Wno-unused-parameter
  -Wno-unused-result
  -Wno-strict-aliasing
  -Wno-misleading-indentation
  -Wno-array-bounds
  -Wno-attribute-warning
  -Wno-maybe-uninitialized
  -Wno-parentheses
  -Wno-type-limits
  -Wno-stringop-overflow
  -Wno-unused-but-set-variable
  -Wno-implicit-fallthrough
  -Wno-void-pointer-to-int-cast
  -Wno-int-to-void-pointer-cast
)

execute_process(
  COMMAND "${VITASDK}/bin/arm-vita-eabi-pkg-config" --libs --static sdl2 SDL2_mixer
  OUTPUT_VARIABLE SONICR_SDL_LIBS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE SONICR_SDL_LIBS_RESULT
)
if(NOT SONICR_SDL_LIBS_RESULT EQUAL 0)
  message(FATAL_ERROR "sdl2 / SDL2_mixer not found for vitasdk. Install them with: vdpm sdl2 sdl2_mixer")
endif()
separate_arguments(SONICR_SDL_LIBS)

execute_process(
  COMMAND "${VITASDK}/bin/arm-vita-eabi-pkg-config" --cflags sdl2 SDL2_mixer
  OUTPUT_VARIABLE SONICR_SDL_CFLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
separate_arguments(SONICR_SDL_CFLAGS)
target_compile_options(${PROJECT_NAME} PRIVATE ${SONICR_SDL_CFLAGS})

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  mathneon
  SceShaccCgExt
  SceShaccCg_stub
  taihen_stub
  SceKernelDmacMgr_stub
  SceCommonDialog_stub
  SceGxm_stub
  SceDisplay_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceNet_stub
  SceNetCtl_stub
  SceSysmodule_stub
  ScePower_stub
  SceProcessmgr_stub
  ${SONICR_SDL_LIBS}
  pthread
  m
)

vita_create_self(eboot.bin ${PROJECT_NAME})

set(SONICR_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../DATA")
file(GLOB_RECURSE SONICR_DATA_FILES CONFIGURE_DEPENDS "${SONICR_DATA_DIR}/*")
set(SONICR_DATA_VPK_FILES "")
foreach(data_file ${SONICR_DATA_FILES})
  file(RELATIVE_PATH data_rel "${SONICR_DATA_DIR}" "${data_file}")
  list(APPEND SONICR_DATA_VPK_FILES FILE "${data_file}" "DATA/${data_rel}")
endforeach()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${SONICR_DATA_VPK_FILES}
)
