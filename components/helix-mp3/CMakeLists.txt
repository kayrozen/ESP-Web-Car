idf_component_register(
    SRCS
        "helix_mp3.c"
        "mp3dec.c"
        "mp3tabs.c"
        "real/bitstream.c"
        "real/buffers.c"
        "real/dct32.c"
        "real/dequant.c"
        "real/dqchan.c"
        "real/huffman.c"
        "real/hufftabs.c"
        "real/imdct.c"
        "real/polyphase.c"
        "real/scalfact.c"
        "real/stproc.c"
        "real/subband.c"
        "real/trigtabs.c"
    INCLUDE_DIRS
        "include"
        "pub"
        "real"
        "."
)

# Suppress warnings from Helix legacy sources
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-maybe-uninitialized
    -Wno-implicit-function-declaration
)
