set(FDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/fdk-aac)

# Decoder-only sources — encoder libs excluded
file(GLOB AACDEC_SRC     "${FDK_ROOT}/libAACdec/src/*.cpp")
file(GLOB ARITH_SRC      "${FDK_ROOT}/libArithCoding/src/*.cpp")
file(GLOB DRCDEC_SRC     "${FDK_ROOT}/libDRCdec/src/*.cpp")
file(GLOB FDK_SRC        "${FDK_ROOT}/libFDK/src/*.cpp")
file(GLOB MPEGTPDEC_SRC  "${FDK_ROOT}/libMpegTPDec/src/*.cpp")
file(GLOB PCMUTILS_SRC   "${FDK_ROOT}/libPCMutils/src/*.cpp")
file(GLOB SBRDEC_SRC     "${FDK_ROOT}/libSBRdec/src/*.cpp")
file(GLOB SYS_SRC        "${FDK_ROOT}/libSYS/src/*.cpp")

idf_component_register(
    SRCS
        "faad2.c"
        ${AACDEC_SRC}
        ${ARITH_SRC}
        ${DRCDEC_SRC}
        ${FDK_SRC}
        ${MPEGTPDEC_SRC}
        ${PCMUTILS_SRC}
        ${SBRDEC_SRC}
        ${SYS_SRC}
    INCLUDE_DIRS
        "include"
        "${FDK_ROOT}/libAACdec/include"
        "${FDK_ROOT}/libSYS/include"
        "${FDK_ROOT}/libArithCoding/include"
        "${FDK_ROOT}/libDRCdec/include"
        "${FDK_ROOT}/libSBRdec/include"
        "${FDK_ROOT}/libMpegTPDec/include"
        "${FDK_ROOT}/libMpegTPEnc/include"
        "${FDK_ROOT}/libPCMutils/include"
        "${FDK_ROOT}/libFDK/include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-implicit-function-declaration
    -Wno-strict-aliasing
    -Wno-maybe-uninitialized
    -Wno-class-memaccess
)
