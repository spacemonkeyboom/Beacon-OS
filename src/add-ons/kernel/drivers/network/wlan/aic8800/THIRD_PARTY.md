# aic8800: third-party code

| File | Licence | Origin |
|---|---|---|
| `if_aic_sdio.c` | GPL-2.0 | Beacon port of the Linux aic8800 driver (functions named in its header comment); Beacon changes are GPL-2.0 too |
| `lmac_msg.h`, `lmac_mac.h` | GPL-2.0 | copied **unchanged** from Linux `drivers/net/wireless/aic/aic8800_fdrv/` |
| `lmac_types.h` | GPL-2.0 | Beacon replacement for the Linux file of that name (which only included Linux headers) |
| `glue.c`, `aic_sdmmc_io.c`, `Jamfile` | MIT | Beacon (glue modelled on Haiku's iaxwifi200 / Beacon broadcomfullmac glue) |
| `../broadcomfullmac/dev/sdmmc/sdmmc_io_haiku.c` (built in via `aic_sdmmc_io.c`) | MIT | Beacon SDIO shim |

Copyright holders of the Linux code: RivieraWaves (2012-2019), AIC
Semiconductor (2020-2024). Their notices are kept in the files.

## Source

Linux driver: "[RFC PATCH wireless-next v4 0/4] wifi: aic: add AIC8800D80
SDIO", Yanli Yang, linux-wireless, 2026-08-29,
Message-Id `<cover.1787991457.git.yanli.yang@bedmex.com>`, fetched from
lore.kernel.org (`.../t.mbox.gz`) on 2026-11-26. Patches 1-3 applied to an
empty tree (patch 4 is Linux build integration only).

sha256 of the patches: 0/4 `ac21795c3fb9bf1599fc2ee070c3d4b2fb094bba0283e2990d96a83ee42e0c9b`,
1/4 `5f2ed92e9ecf60e8de83935e32d1368e914a31bde87f6269e2816830c4dbb649`,
2/4 `336b935c86e42470649496538f2e90109eab53416a145802e86bbf7a67a8489d`,
3/4 `9709249d3a31155c70d1b3aacbbae3bd20a3d6e11e1514414d0599a8461db9b0`.

sha256 of the files used (as applied):

```
6ff4fdf36eb9a98a86fc96e35bca06f47991647339e5f4721c34fa4b871913ab  aic8800_bsp/aicsdio.c
8a13432624fdae463fce018a0a5e8872cda4d54c1a7214af5f4fafc9ffcd24d0  aic8800_bsp/aicsdio_txrxif.c
4421cd7d52fc8a916c52e0428749ae2cbfb3c58a42827a3a3b49a159d1c4cb16  aic8800_bsp/aic_bsp_driver.c
c7a70d438f5b88f9082b1483b7c5d196431a46c7e03e07a0135d032a3a8d5e9b  aic8800_bsp/aic_chip_8800d80.c
f17a308a6e93eeace2bdd21238b203167f4b7678f73266c1d32f1be41eb7903b  aic8800_bsp/aic8800d80_compat.c
863a32078f776cc7d51465df2ed35d92a4cf8c13a59c5640a21d84750576938c  aic8800_fdrv/rwnx_msg_tx.c
789ddc35fcee2b0e278a036812945b8f0dc4de35907c3ededf53ce12115defb5  aic8800_fdrv/lmac_msg.h
ae92ad315b9d4a4294fa3cc841d5e954cc3110b6b26a0161d9a2c4c15eef6575  aic8800_fdrv/lmac_mac.h
cb109fe390ea2c675194ba74ccfe6f0f78930381120782185cd57f4417481e58  aic8800_fdrv/aicwf_chip_8800d80.c
8b61d4a49c8dca17c9329585624d82d52e3173bc784817faf05254c0fb401d93  aic8800_fdrv/aicwf_compat_8800d80.c
d54e812ae04c89d7a32ab5c2330a5594145e9506c35cce49802525d193d0f398  aic8800_fdrv/rwnx_platform.c
ac5d42b61efb841a55415d64b746b60dbdee4764c29c7b24a03dfa915c733dd6  aic8800_fdrv/rwnx_main.c
cbf408efbce7a00de5f27aae0f2eb017763f9227340d16af5aa75baf73eca5cd  aic8800_fdrv/rwnx_tx.c
ddbcea2edcffa15b4db4e794a3cb4704f30d1e995ac9274e84c19416e773a792  aic8800_fdrv/rwnx_rx.c
ec6c717520a74e0b614ada5091705fd4bd2a040eb5b0400db2d23534cffb0d88  aic8800_fdrv/rwnx_rx.h
8dc884163ac3d735c89b33150ca23ad4052feabd4c184ab0b9d80ff0359c8782  aic8800_fdrv/ipc_shared.h
d3dfe4189432e2cb0777ef0ec438c295e32d79cba39e293fa66991de838745ce  aic8800_fdrv/aicwf_sdio.c
b32e4c977f99321f650a684eb8dcabb2b681002ca00e8df892fac3ad35904eb5  aic8800_fdrv/aicwf_txrxif.c
e21e7b1df1831d000f002824cd88424d271933dd6e4307755b168cef692390fd  aic8800_fdrv/aicwf_chip_ops.c
9f23100258fb99209120c2d40343eaac45c4e1f8591ea2475032e60240513db7  aic8800_fdrv/rwnx_msg_rx.c
6d61b23a5a7c52d28b07e21975af2cd6c04e203018fd07207bef01d74b917e76  aic8800_fdrv/rwnx_mod_params.c
```

Reference only (board wiring, not code): StarFive `linux`
`JH7110_VisionFive2_6.12.y_devel` at `4cecf169f3` (VF2 Lite device tree,
`dw_mmc-starfive.c` WL_REG_ON handling).

## Firmware: not included

The chip needs AIC's firmware (`fmacfw_8800d80_u02.bin`). AIC has not given
redistribution permission (the upstream RFC says so too), so Beacon ships no
firmware, in the repo or in public images. The driver loads it at run time
from `/boot/system/non-packaged/data/firmware/aic8800/` (or the packaged
`/boot/system/data/firmware/aic8800/`).

## Also consulted (revision handling)

radxa-pkg/aic8800 (GitHub), commit `5562c254b0231be34588dc608ec28c4f1d613285`,
src/SDIO/driver_fw/driver/aic8800/aic8800_bsp/ (GPL-2.0): `aic_bsp_driver.c`
`aicbsp_driver_fw_init()` and `aic_bsp_driver.h` (CHIP_REV_U03 = CHIP_REV_U04
= 7). The D80 U03/U04 runs the same U02 path and firmware set; the
revision check in `if_aic_sdio.c` follows that.
