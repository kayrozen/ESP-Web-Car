idf_component_register(
    SRCS
        "speexdsp_resampler.c"
        "libspeexdsp/resample.c"
    INCLUDE_DIRS
        "include"
        "libspeexdsp"
)

# Use fixed-point (integer) path — no FPU required, lower CPU than float
# OUTSIDE_SPEEX: compile resample.c standalone without the full Speex codec
# RANDOM_PREFIX: namespaces all symbols to avoid conflicts (speex_*)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OUTSIDE_SPEEX=1
    RANDOM_PREFIX=speex
    FIXED_POINT=1
    EXPORT=
    RESAMPLE_FULL_SINC_TABLE=0
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-implicit-function-declaration
)
