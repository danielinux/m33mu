# STM32H5 flash TrustZone filter — silicon ground truth

`main.c` reads marker words either side of the secure watermark through both
flash aliases and reports what the flash TZ filter let through. The same binary
runs on a NUCLEO-H563ZI and under m33mu, so the transcripts can be diffed.
`scripts/flash-tz-hw-parity.sh` does exactly that, and ctest runs it as
`flash_tz_hw_parity_test`.

`nucleo-h563zi.expected.txt` is the reference, captured over the board's VCP.
`nucleo-h563zi.raw.txt` is the unedited capture; the reference drops the CRs and
the trailing `HARDFAULT`, which is only the final `bkpt` escalating with no
debugger attached.

## Board configuration

Read back with `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -ob displ`:

    TZEN         : 0xB4 (Trust zone enabled)
    SECWM1_STRT  : 0x00   SECWM1_END : 0x0F     -> bank 1 sectors 0..15 secure
    SECWM2_STRT  : 0x7E   SECWM2_END : 0x7F     -> bank 2 sectors 126..127 secure

and with the core held in reset, `SECBB1R[0..7]` and `SECBB2R[0..7]` all read
zero. Block-based attribution contributes nothing out of reset; the watermark
is the whole story until firmware writes SECBB.

## What the transcript establishes

1. **There is no unfiltered state.** With SECWM1 = 0..15 and SECBB clear,
   sector 20 reads `00000000` through the secure alias and its marker through
   the non-secure one. A sector outside every watermark is simply non-secure,
   and a secure access to it reads back zero. It is not "unprovisioned,
   therefore pass everything".

2. **The transaction attribute comes from SAU/IDAU, not from the alias.** In
   the `sau=off` block both aliases read the same values, because with SAU
   disabled every address is Secure and even a `0x08...` read issues a secure
   transaction. That is the state out of reset.

3. **SECBB is cumulative with the watermark.** Setting `SECBB1R1` bit 20 flips
   sector 20 to secure — secure alias returns the marker, non-secure alias
   returns zero — and clearing it flips it back.

4. **Clearing SECBB does not strip the watermark.** Sector 4 stays secure
   across `secbb-cleared`. wolfBoot's `hal_tz_release_nonsecure_area()` relies
   on this.

5. Rejection is silent in both directions: no bus fault, just zero.

## Superseded workaround

Before this was measured, `flash_tz_filter_armed()` in `cpu/stm32h5_mmio.c`
modelled an unprovisioned device — SECWM empty, SECBB clear — as *unfiltered*,
so that a bare secure image could boot without a watermark. It armed the filter
per bank as soon as any SECBB bit in that bank was set.

That is wrong in two ways, and point 1 above is why: silicon has no unfiltered
state, and arming is not a per-bank property. It also broke wolfBoot's stm32h5
TrustZone update path. `hal_tz_claim_nonsecure_area()` sets one SECBB1 bit to
write the boot partition, which armed the bank; every sector outside the empty
watermark — including sector 0, which the bootloader was executing from — then
became non-secure, and wolfBoot's next literal-pool read returned zero.

The minimal fix was to arm on a programmed watermark only:

```diff
 static mm_bool flash_tz_filter_armed(struct flash_state *f, mm_bool phys_hi)
 {
     secwm = f->regs[(phys_hi ? FLASH_SECWM2R_CUR : FLASH_SECWM1R_CUR) / 4u];
-    if (secwm != FLASH_SECWM_EMPTY) {
-        return MM_TRUE;
-    }
-    bb_base = phys_hi ? FLASH_SECBB2R : FLASH_SECBB1R;
-    for (i = 0; i < FLASH_SECBB_NREGS; i++) {
-        if (f->regs[(bb_base + 4u * i) / 4u] != 0u) {
-            return MM_TRUE;
-        }
-    }
-    return MM_FALSE;
+    return (secwm != FLASH_SECWM_EMPTY) ? MM_TRUE : MM_FALSE;
 }
```

It restored wolfBoot and kept the suite green, but it still modelled a
watermark-less part as unfiltered, which no real part does. It is recorded here
as the reference point the current model replaced, not as something to go back
to.

## What replaced it

The arming heuristic is gone. The watermark is now provisioned explicitly, the
way a board is provisioned before an image is flashed onto it:

    m33mu --cpu stm32h563 --secwm1=0:47 --secwm2=0:127 ...

and the filter then behaves exactly as measured above, in both directions, with
`MM_FLASH_TZ_UNFILTERED` reserved for TrustZone-disabled sessions (`--no-tz`, or
a non-secure image detected at boot).

A session that provisions nothing leaves the filter inert and prints

    [FLASH] SECWM not provisioned; flash TZ filter inert.

rather than guessing. There is no guess to make: which sectors are secure is a
property of the option bytes, not of the image, and the two shapes that run here
want opposite answers. `test-stm32h563-dualbank` is a secure image occupying
both banks, so it wants every sector secure; wolfBoot is a secure bootloader
handing off to a non-secure application, so it wants the sectors above its own
non-secure. A default that satisfies one breaks the other, and it breaks it as a
silent zero read a long way from the cause — which is exactly how the original
regression presented.

So the fallback is deliberately the pre-filter behaviour, and provisioning is
the opt-in. wolfBoot's stm32h5 TrustZone configs, for the record, correspond to
`--secwm1=0:47 --secwm2=0:127`: the bootloader occupies sectors 0..47, the boot
partition after it must be non-secure for the application to run, and bank 2 is
secure because the update partition lives in the secure alias at `0x0C100000`.
