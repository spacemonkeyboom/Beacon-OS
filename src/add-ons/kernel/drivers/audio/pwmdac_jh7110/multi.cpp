/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * multi_audio (hmulti) interface of the JH7110 PWM-DAC driver: a stereo,
 * playback-only, 16-bit device with no hardware mixer. Follows the
 * structure of Haiku's null_audio and hda drivers (MIT), but copies every
 * ioctl structure with user_memcpy(): on riscv64 the kernel must not
 * dereference user pointers directly (sstatus.SUM is clear).
 *
 * This file contains no third-party code.
 */


#include "Pwmdac.h"

#include <string.h>

#include <kernel.h>


static const multi_channel_info kChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
};

#define MIX_GROUP_PLAYBACK_ID	1024


template<typename T>
static status_t
copy_in(T& local, const void* user)
{
	if (user == NULL || !IS_USER_ADDRESS(user))
		return B_BAD_ADDRESS;
	return user_memcpy(&local, user, sizeof(T));
}


template<typename T>
static status_t
copy_out(void* user, const T& local)
{
	if (user == NULL || !IS_USER_ADDRESS(user))
		return B_BAD_ADDRESS;
	return user_memcpy(user, &local, sizeof(T));
}


static status_t
get_description(PwmdacDevice* device, void* arg)
{
	multi_description description;
	if (copy_in(description, arg) != B_OK)
		return B_BAD_ADDRESS;

	int32 requested = description.request_channel_count;
	multi_channel_info* userChannels = description.channels;

	// memset also zeroes the float cvsr fields without FP code.
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name, "JH7110 PWM-DAC",
		sizeof(description.friendly_name));
	strlcpy(description.vendor_info, "Beacon (StarFive)",
		sizeof(description.vendor_info));
	description.output_channel_count = 2;
	description.input_channel_count = 0;
	description.output_bus_channel_count = 2;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;
	description.request_channel_count = requested;
	description.channels = userChannels;
	description.output_rates = device->SupportedRates();
	description.input_rates = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 30000;
	description.control_panel[0] = '\0';

	if (copy_out(arg, description) != B_OK)
		return B_BAD_ADDRESS;

	if (requested >= (int32)B_COUNT_OF(kChannels)) {
		if (userChannels == NULL || !IS_USER_ADDRESS(userChannels)
			|| user_memcpy(userChannels, kChannels, sizeof(kChannels))
				!= B_OK) {
			return B_BAD_ADDRESS;
		}
	}
	return B_OK;
}


static status_t
get_enabled_channels(PwmdacDevice* device, void* arg)
{
	multi_channel_enable enable;
	if (copy_in(enable, arg) != B_OK)
		return B_BAD_ADDRESS;
	uchar bits = 0;
	B_SET_CHANNEL(&bits, 0, true);
	B_SET_CHANNEL(&bits, 1, true);
	B_SET_CHANNEL(&bits, 2, true);
	B_SET_CHANNEL(&bits, 3, true);
	if (enable.enable_bits != NULL) {
		if (!IS_USER_ADDRESS(enable.enable_bits)
			|| user_memcpy(enable.enable_bits, &bits, 1) != B_OK)
			return B_BAD_ADDRESS;
	}
	enable.lock_source = B_MULTI_LOCK_INTERNAL;
	return copy_out(arg, enable);
}


static status_t
set_global_format(PwmdacDevice* device, void* arg)
{
	multi_format_info format;
	if (copy_in(format, arg) != B_OK)
		return B_BAD_ADDRESS;
	if (format.output.format != 0 && format.output.format != B_FMT_16BIT) {
		ERROR("%s: format 0x%" B_PRIx32 " not supported (16-bit only)\n",
			device->Name(), format.output.format);
		return B_BAD_VALUE;
	}
	if (format.output.rate == 0)
		return B_OK;
	status_t status = device->SetRate(format.output.rate);
	if (status != B_OK) {
		ERROR("%s: rate code 0x%" B_PRIx32 " not supported\n", device->Name(),
			format.output.rate);
	}
	return status;
}


static status_t
get_global_format(PwmdacDevice* device, void* arg)
{
	multi_format_info format;
	if (copy_in(format, arg) != B_OK)
		return B_BAD_ADDRESS;
	size_t infoSize = format.info_size;
	memset(&format, 0, sizeof(format));
	format.info_size = infoSize;
	format.output_latency = device->LatencyUs();
	format.input_latency = 0;
	format.timecode_kind = 0;
	format.output.rate = device->RateCode();
	format.output.format = B_FMT_16BIT;
	return copy_out(arg, format);
}


static status_t
list_mix_controls(PwmdacDevice* device, void* arg)
{
	multi_mix_control_info info;
	if (copy_in(info, arg) != B_OK)
		return B_BAD_ADDRESS;

	int32 available = info.control_count;
	info.control_count = 0;
	if (available >= 1 && info.controls != NULL) {
		multi_mix_control control;
		memset(&control, 0, sizeof(control));
		control.id = MIX_GROUP_PLAYBACK_ID;
		control.flags = B_MULTI_MIX_GROUP;
		control.parent = 0;
		control.string = S_null;
		strlcpy(control.name, "Headphone jack", sizeof(control.name));
		if (copy_out(info.controls, control) != B_OK)
			return B_BAD_ADDRESS;
		info.control_count = 1;
	}
	return copy_out(arg, info);
}


static status_t
get_mix(PwmdacDevice* device, void* arg)
{
	// No hardware gain; the media mixer does volume in software.
	multi_mix_value_info info;
	if (copy_in(info, arg) != B_OK)
		return B_BAD_ADDRESS;
	info.item_count = 0;
	return copy_out(arg, info);
}


static status_t
get_buffers(PwmdacDevice* device, void* arg)
{
	multi_buffer_list list;
	if (copy_in(list, arg) != B_OK)
		return B_BAD_ADDRESS;

	int32 count = list.request_playback_buffers;
	if (count < PWMDAC_MIN_BUFFERS || count > PWMDAC_MAX_BUFFERS)
		count = PWMDAC_MIN_BUFFERS;
	uint32 frames = list.request_playback_buffer_size;
	if (frames == 0)
		frames = PWMDAC_DEFAULT_FRAMES;
	if (frames < PWMDAC_MIN_FRAMES)
		frames = PWMDAC_MIN_FRAMES;
	if (frames > PWMDAC_MAX_FRAMES)
		frames = PWMDAC_MAX_FRAMES;
	if (list.request_playback_channels != 0
		&& list.request_playback_channels != 2) {
		INFO("%s: %" B_PRId32 " playback channels requested, giving 2\n",
			device->Name(), list.request_playback_channels);
	}

	status_t status = device->CreateBuffers(count, frames);
	if (status != B_OK)
		return status;

	list.flags = B_MULTI_BUFFER_PLAYBACK;
	list.return_playback_buffers = count;
	list.return_playback_channels = 2;
	list.return_playback_buffer_size = frames;
	list.return_record_buffers = 0;
	list.return_record_channels = 0;
	list.return_record_buffer_size = 0;

	if (list.playback_buffers == NULL
		|| !IS_USER_ADDRESS(list.playback_buffers))
		return B_BAD_ADDRESS;
	for (int32 i = 0; i < count; i++) {
		buffer_desc* userDescs;
		if (user_memcpy(&userDescs, &list.playback_buffers[i],
				sizeof(userDescs)) != B_OK
			|| userDescs == NULL || !IS_USER_ADDRESS(userDescs)) {
			return B_BAD_ADDRESS;
		}
		buffer_desc descs[2];
		char* base = (char*)device->UserBuffer(i);
		for (int32 c = 0; c < 2; c++) {
			descs[c].base = base + 2 * c;
			descs[c].stride = PWMDAC_FRAME_BYTES;
		}
		if (user_memcpy(userDescs, descs, sizeof(descs)) != B_OK)
			return B_BAD_ADDRESS;
	}

	return copy_out(arg, list);
}


static status_t
buffer_exchange(PwmdacDevice* device, void* arg)
{
	multi_buffer_info info;
	if (copy_in(info, arg) != B_OK)
		return B_BAD_ADDRESS;

	bigtime_t realTime;
	int64 frames;
	int32 cycle;
	status_t status = device->WaitBuffer(realTime, frames, cycle);
	if (status != B_OK)
		return status;

	// As in hda: playback_buffer_cycle is the buffer now being played;
	// MultiAudioNode fills (cycle - 1).
	info.flags = B_MULTI_BUFFER_PLAYBACK;
	info.played_real_time = realTime;
	info.played_frames_count = frames;
	info.playback_buffer_cycle = cycle;
	info.recorded_real_time = 0;
	info.recorded_frames_count = 0;
	info.record_buffer_cycle = 0;
	return copy_out(arg, info);
}


status_t
pwmdac_multi_control(PwmdacDevice* device, uint32 op, void* arg,
	size_t length)
{
	switch (op) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description(device, arg);
		case B_MULTI_GET_EVENT_INFO:
		case B_MULTI_SET_EVENT_INFO:
		case B_MULTI_GET_EVENT:
			return B_ERROR;
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels(device, arg);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format(device, arg);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format(device, arg);
		case B_MULTI_GET_CHANNEL_FORMATS:
		case B_MULTI_SET_CHANNEL_FORMATS:
			return B_ERROR;
		case B_MULTI_GET_MIX:
			return get_mix(device, arg);
		case B_MULTI_SET_MIX:
			return B_OK;
		case B_MULTI_LIST_MIX_CHANNELS:
		case B_MULTI_LIST_MIX_CONNECTIONS:
			return B_ERROR;
		case B_MULTI_LIST_MIX_CONTROLS:
			return list_mix_controls(device, arg);
		case B_MULTI_GET_BUFFERS:
			return get_buffers(device, arg);
		case B_MULTI_SET_BUFFERS:
		case B_MULTI_SET_START_TIME:
			return B_ERROR;
		case B_MULTI_BUFFER_EXCHANGE:
			return buffer_exchange(device, arg);
		case B_MULTI_BUFFER_FORCE_STOP:
			device->Stop();
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}
