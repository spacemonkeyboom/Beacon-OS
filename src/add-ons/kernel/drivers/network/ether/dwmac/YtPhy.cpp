/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Motorcomm YT8531 PHY support for the Beacon dwmac driver.
 *
 * Adapted from the StarFive/Orange Pi vendor U-Boot drivers/net/phy/
 * motorcomm.c (orangepi-xunlong/u-boot-orangepi v2021.10-jh7110, commit
 * d7274dfe), GPL-2.0+:
 *   Copyright 2010-2011 Freescale Semiconductor, Inc.
 *   author Andy Fleming
 * (file header says "RealTek PHY drivers"; it is the YuTai/Motorcomm one)
 *
 * Extended register access, the DT tuning fields (rxc_dly_en, rgmii_sw_dr*,
 * rx_delay_sel, tx_delay_sel*, tx_inverted_*), their bit positions and
 * defaults, and the status register decode come from ytphy_read_ext,
 * ytphy_write_ext, ytphy_of_config, ytphy_of_inverted and
 * ytphy_parse_status there.
 *
 * Modified for Beacon/Haiku, 2026 (The Beacon Project).
 */


#include "Dwmac.h"


#define INFO(x...)	dprintf("dwmac: " x)


enum {
	kYtSpecStatus		= 0x11,
	kYtExtAddr			= 0x1e,
	kYtExtData			= 0x1f,

	kYtExtChipConfig	= 0xa001,
	kYtExtRgmiiConfig1	= 0xa003,
	kYtExtPadDrive		= 0xa010,

	kYtStatusDuplex		= 1u << 13,
	kYtStatusSpeedShift	= 14,
	kYtStatusSpeedMask	= 3,
	kYtStatusResolved	= 1u << 11,
	kYtStatusLinkRt		= 1u << 10,
};


struct yt_field {
	const char*	name;
	uint8		size;
	uint8		offset;
	uint8		dflt;
};

// vendor motorcomm.c ytphy_dr_grp / ytphy_rxtxd_grp / ytphy_rxden_grp
static const yt_field kDriveFields[] = {
	{ "rgmii_sw_dr", 2, 4, 0x3 },
	{ "rgmii_sw_dr_2", 1, 12, 0x0 },
	{ "rgmii_sw_dr_rxc", 3, 13, 0x3 },
};

static const yt_field kDelayFields[] = {
	{ "rx_delay_sel", 4, 10, 0x0 },
	{ "tx_delay_sel_fe", 4, 4, 0xf },
	{ "tx_delay_sel", 4, 0, 0x1 },
};

static const yt_field kRxcDelayField = { "rxc_dly_en", 1, 8, 0x1 };
static const uint8 kTxInvertedOffset = 14;


static int32
yt_read_ext(DwmacDevice* device, int32 phy, uint16 reg)
{
	if (device->MdioWrite(phy, kYtExtAddr, reg) != B_OK)
		return -1;
	return device->MdioRead(phy, kYtExtData);
}


static status_t
yt_write_ext(DwmacDevice* device, int32 phy, uint16 reg, uint16 value)
{
	status_t status = device->MdioWrite(phy, kYtExtAddr, reg);
	if (status != B_OK)
		return status;
	return device->MdioWrite(phy, kYtExtData, value);
}


static uint32
field_replace(uint32 value, uint8 offset, uint8 size, uint32 field)
{
	uint32 mask = ((1u << size) - 1) << offset;
	return (value & ~mask) | ((field << offset) & mask);
}


static uint32
field_value(int32 configured, const yt_field& field)
{
	// vendor: absent -> default; too large -> clamp to the field maximum
	uint32 value = configured >= 0 ? (uint32)configured : field.dflt;
	uint32 max = (1u << field.size) - 1;
	return value > max ? max : value;
}


void
yt8531_log_state(DwmacDevice* device, int32 phy, const char* when)
{
	INFO("%s: YT8531 %s: chip_config(a001) %#06" B_PRIx32 " rgmii_config1"
		"(a003) %#06" B_PRIx32 " pad_drive(a010) %#06" B_PRIx32 " spec_status"
		"(11) %#06" B_PRIx32 "\n", device->Name(), when,
		yt_read_ext(device, phy, kYtExtChipConfig),
		yt_read_ext(device, phy, kYtExtRgmiiConfig1),
		yt_read_ext(device, phy, kYtExtPadDrive),
		device->MdioRead(phy, kYtSpecStatus));
}


void
yt8531_configure(DwmacDevice* device, int32 phy, const YtPhyConfig& config)
{
	// vendor ytphy_of_config()
	if (config.rxcDlyEn >= 0) {
		int32 value = yt_read_ext(device, phy, kYtExtChipConfig);
		if (value >= 0) {
			value = field_replace(value, kRxcDelayField.offset,
				kRxcDelayField.size, field_value(config.rxcDlyEn,
					kRxcDelayField));
			yt_write_ext(device, phy, kYtExtChipConfig, (uint16)value);
		}
	}

	const int32 drive[] = { config.rgmiiSwDr, config.rgmiiSwDr2,
		config.rgmiiSwDrRxc };
	int32 value = yt_read_ext(device, phy, kYtExtPadDrive);
	if (value >= 0) {
		for (int i = 0; i < 3; i++) {
			value = field_replace(value, kDriveFields[i].offset,
				kDriveFields[i].size, field_value(drive[i], kDriveFields[i]));
		}
		yt_write_ext(device, phy, kYtExtPadDrive, (uint16)value);
	}

	const int32 delay[] = { config.rxDelaySel, config.txDelaySelFe,
		config.txDelaySel };
	value = yt_read_ext(device, phy, kYtExtRgmiiConfig1);
	if (value >= 0) {
		for (int i = 0; i < 3; i++) {
			value = field_replace(value, kDelayFields[i].offset,
				kDelayFields[i].size, field_value(delay[i], kDelayFields[i]));
		}
		yt_write_ext(device, phy, kYtExtRgmiiConfig1, (uint16)value);
	}

	INFO("%s: YT8531 DT tuning: rxc_dly_en %" B_PRId32 " sw_dr %" B_PRId32
		" sw_dr_2 %" B_PRId32 " sw_dr_rxc %" B_PRId32 " rx_delay_sel %"
		B_PRId32 " tx_delay_sel_fe %" B_PRId32 " tx_delay_sel %" B_PRId32
		" tx_inverted 10/100/1000 %" B_PRId32 "/%" B_PRId32 "/%" B_PRId32
		" (-1 = not in DT, vendor default used)\n", device->Name(),
		config.rxcDlyEn, config.rgmiiSwDr, config.rgmiiSwDr2,
		config.rgmiiSwDrRxc, config.rxDelaySel, config.txDelaySelFe,
		config.txDelaySel, config.txInverted10, config.txInverted100,
		config.txInverted1000);
}


void
yt8531_set_tx_inverted(DwmacDevice* device, int32 phy,
	const YtPhyConfig& config, uint32 speed)
{
	// vendor ytphy_of_inverted(): one bit (14) of RGMII_CONFIG1, value per
	// link speed, absent = 0
	int32 inverted = speed == 1000 ? config.txInverted1000
		: speed == 100 ? config.txInverted100 : config.txInverted10;
	if (inverted < 0)
		inverted = 0;
	int32 value = yt_read_ext(device, phy, kYtExtRgmiiConfig1);
	if (value < 0)
		return;
	int32 updated = field_replace(value, kTxInvertedOffset, 1,
		inverted != 0 ? 1 : 0);
	if (updated != value)
		yt_write_ext(device, phy, kYtExtRgmiiConfig1, (uint16)updated);
	INFO("%s: YT8531 tx_inverted for %" B_PRIu32 " Mbit/s = %" B_PRId32
		" (rgmii_config1 %#06" B_PRIx32 ")\n", device->Name(), speed, inverted,
		updated);
}


bool
yt8531_read_status(DwmacDevice* device, int32 phy, bool& up, uint32& speed,
	bool& fullDuplex)
{
	// vendor ytphy_startup(): genphy_update_link() then ytphy_parse_status()
	device->MdioRead(phy, kMiiBmsr);
	int32 bmsr = device->MdioRead(phy, kMiiBmsr);
	int32 status = device->MdioRead(phy, kYtSpecStatus);
	if (bmsr < 0 || status < 0)
		return false;

	up = (bmsr & kBmsrLink) != 0 && (bmsr & kBmsrAnComplete) != 0
		&& (status & kYtStatusResolved) != 0;
	fullDuplex = (status & kYtStatusDuplex) != 0;
	switch ((status >> kYtStatusSpeedShift) & kYtStatusSpeedMask) {
		case 2:
			speed = 1000;
			break;
		case 1:
			speed = 100;
			break;
		default:
			speed = 10;
			break;
	}
	return true;
}
