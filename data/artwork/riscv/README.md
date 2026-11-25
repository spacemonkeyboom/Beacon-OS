<!-- SPDX-License-Identifier: MIT -->
# RISC-V logo (boot screen)

`RISC-V_Stacked_White_Yellow.{png,svg}` are RISC-V International's official
stacked "White & Yellow" logo files, unmodified, downloaded on 2026-11-25
from https://riscv.org/about/risc-v-branding-guidelines/
(`Secondary-Logo_RISC-V-Stacked-20230728T144248Z-001.zip`).

**RISC-V and the RISC-V logo are registered trademarks of RISC-V
International.** Beacon is not affiliated with or endorsed by RISC-V
International. They are used here under RISC-V International's brand
guidelines, which allow free use by non-commercial projects that follow the
guidelines: only official logo files, never redrawn or modified, and the
White version on dark backgrounds.

The riscv64 EFI boot loader shows the logo on RISC-V's Berkeley Blue
(#003262) right after it starts, replacing the firmware's logo.
`make_boot_logo.py` (MIT) generates
`src/system/boot/platform/efi/arch/riscv64/riscv_boot_logo.h`. It only
scales the logo and composites it onto the blue.

These files are not covered by Beacon's MIT licence.
