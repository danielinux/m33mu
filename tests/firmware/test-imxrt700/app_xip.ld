/* CPU0 secure image executed in place from XSPI0 (secure alias).  The FCB
 * occupies the first 16 KB of flash; the image starts at offset 0x4000. */
MEMORY
{
    CODE (rx)  : ORIGIN = 0x38004000, LENGTH = 0x00100000
    RAM  (rwx) : ORIGIN = 0x30080000, LENGTH = 0x00080000
}
_image_exec_addr = 0;
INCLUDE sections.ld
