file(GLOB FAAD2_SRCS "libfaad/*.c")

idf_component_register(
    SRCS
        "faad2.c"
        ${FAAD2_SRCS}
    INCLUDE_DIRS
        "include"
        "libfaad"
)

# SBR disabled by default — saves ~30% CPU critical for WiFi+BT coexistence
if(CONFIG_AAC_DISABLE_SBR)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SBR_DEC=0
        PS_DEC=0
    )
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SBR_DEC=1
        PS_DEC=1
    )
endif()

# FAAD2 uses LGPL — link as component (IDF creates static libs per component,
# which satisfies LGPL §6 via the .a archive being separately available)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_STDINT_H=1
    FIXED_POINT=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-implicit-function-declaration
    -Wno-strict-aliasing
    -Wno-array-bounds
    -Wno-maybe-uninitialized
    -Wno-pointer-sign
)
