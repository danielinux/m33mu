/* CPU0 secure load-to-RAM image: the boot ROM copies it from XSPI0 offset
 * 0x4000 to 0x30100000 (above the 84 KB the ROM owns) and starts it there. */
MEMORY
{
    CODE (rwx) : ORIGIN = 0x30100000, LENGTH = 0x00080000
    RAM  (rwx) : ORIGIN = 0x30180000, LENGTH = 0x00080000
}
_image_exec_addr = ORIGIN(CODE);
INCLUDE sections.ld
