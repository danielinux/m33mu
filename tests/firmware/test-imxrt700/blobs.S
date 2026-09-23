/* CPU1 and non-secure images embedded into the CPU0 test image. */
    .section .blobs, "a"
    .balign 4
    .global core1_bin_start
    .global core1_bin_end
core1_bin_start:
    .incbin "core1/core1.bin"
core1_bin_end:
    .balign 4
    .global ns_bin_start
    .global ns_bin_end
ns_bin_start:
    .incbin "ns/ns.bin"
ns_bin_end:
    .balign 4
