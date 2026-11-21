/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * hmulti_tone: play a sine tone straight through a multi_audio (hmulti)
 * driver, without the media server. Written to test the JH7110 PWM-DAC
 * driver, works with any 16-bit stereo hmulti device.
 *
 *   hmulti_tone [-d device] [-f hz] [-t seconds] [-r 44100|48000]
 *               [-c left|right|both] [-a amplitude%]
 *
 * Only one program can stream to the device at a time: stop the media
 * server first (quit application/x-vnd.Be.media-server). On the JH7110
 * audio driver the tone goes to the output chosen with audio_output
 * (headphones or HDMI).
 */


#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <hmulti_audio.h>


static const char* kDefaultDevice = "/dev/audio/hmulti/pwmdac/1";


static void
usage()
{
	fprintf(stderr, "usage: hmulti_tone [-d device] [-f hz] [-t seconds] "
		"[-r 44100|48000] [-c left|right|both] [-a amplitude%%]\n"
		"default: -d %s -f 440 -t 3 -r 48000 -c both -a 50\n",
		kDefaultDevice);
	exit(1);
}


int
main(int argc, char** argv)
{
	const char* path = kDefaultDevice;
	double frequency = 440;
	double seconds = 3;
	uint32 rate = 48000;
	bool left = true, right = true;
	double amplitude = 0.5;

	int opt;
	while ((opt = getopt(argc, argv, "d:f:t:r:c:a:h")) != -1) {
		switch (opt) {
			case 'd': path = optarg; break;
			case 'f': frequency = atof(optarg); break;
			case 't': seconds = atof(optarg); break;
			case 'r': rate = strtoul(optarg, NULL, 10); break;
			case 'c':
				left = strcmp(optarg, "right") != 0;
				right = strcmp(optarg, "left") != 0;
				break;
			case 'a': amplitude = atof(optarg) / 100.0; break;
			default: usage();
		}
	}
	uint32 rateCode = rate == 44100 ? B_SR_44100 : B_SR_48000;
	if (rate != 44100 && rate != 48000)
		usage();
	if (amplitude < 0 || amplitude > 1)
		amplitude = 0.5;

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "hmulti_tone: cannot open %s: %s\n", path,
			strerror(errno));
		if (errno == EBUSY) {
			fprintf(stderr, "  the media server probably has it open: run "
				"'quit application/x-vnd.Be.media-server' first\n");
		}
		return 1;
	}

	multi_description description;
	multi_channel_info channels[16];
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = 16;
	description.channels = channels;
	if (ioctl(fd, B_MULTI_GET_DESCRIPTION, &description, sizeof(description))
			!= 0) {
		fprintf(stderr, "hmulti_tone: GET_DESCRIPTION: %s\n", strerror(errno));
		return 1;
	}
	printf("device: %s (%s), %" B_PRId32 " out channels, rates 0x%" B_PRIx32
		", formats 0x%" B_PRIx32 "\n", description.friendly_name,
		description.vendor_info, description.output_channel_count,
		description.output_rates, description.output_formats);
	if ((description.output_formats & B_FMT_16BIT) == 0
		|| description.output_channel_count < 2) {
		fprintf(stderr, "hmulti_tone: needs a 16-bit stereo output\n");
		return 1;
	}

	multi_format_info format;
	memset(&format, 0, sizeof(format));
	format.info_size = sizeof(format);
	format.output.rate = rateCode;
	format.output.format = B_FMT_16BIT;
	if (ioctl(fd, B_MULTI_SET_GLOBAL_FORMAT, &format, sizeof(format)) != 0) {
		if (errno == EBUSY) {
			fprintf(stderr, "hmulti_tone: %s is streaming for another program;"
				" the media server probably has it: run 'quit "
				"application/x-vnd.Be.media-server' first\n", path);
			return 1;
		}
		fprintf(stderr, "hmulti_tone: SET_GLOBAL_FORMAT %" B_PRIu32 " Hz: %s\n",
			rate, strerror(errno));
		return 1;
	}

	buffer_desc descStorage[8][2];
	buffer_desc* descs[8];
	for (int i = 0; i < 8; i++)
		descs[i] = descStorage[i];
	multi_buffer_list list;
	memset(&list, 0, sizeof(list));
	list.info_size = sizeof(list);
	list.request_playback_buffers = 2;
	list.request_playback_channels = 2;
	list.request_playback_buffer_size = 0;
	list.playback_buffers = descs;
	if (ioctl(fd, B_MULTI_GET_BUFFERS, &list, sizeof(list)) != 0) {
		fprintf(stderr, "hmulti_tone: GET_BUFFERS: %s\n", strerror(errno));
		return 1;
	}
	int32 bufferCount = list.return_playback_buffers;
	uint32 frames = list.return_playback_buffer_size;
	printf("buffers: %" B_PRId32 " x %" B_PRIu32 " frames, stride %" B_PRIu32
		"\n", bufferCount, frames, (uint32)descs[0][0].stride);

	double phase = 0;
	double step = 2 * M_PI * frequency / rate;
	int64 total = (int64)(seconds * rate);
	int64 queued = 0;

	// Fill all buffers once, then refill (cycle - 1) after each exchange.
	auto fill = [&](int32 index) {
		for (uint32 f = 0; f < frames; f++) {
			int16 value = 0;
			if (queued < total)
				value = (int16)(sin(phase) * amplitude * 32767);
			phase += step;
			if (phase > 2 * M_PI)
				phase -= 2 * M_PI;
			queued++;
			*(int16*)(descs[index][0].base + f * descs[index][0].stride)
				= left ? value : 0;
			*(int16*)(descs[index][1].base + f * descs[index][1].stride)
				= right ? value : 0;
		}
	};
	for (int32 i = 0; i < bufferCount; i++)
		fill(i);

	multi_buffer_info info;
	bigtime_t firstTime = 0;
	int64 firstFrames = 0;
	int64 exchanges = 0;
	bigtime_t start = system_time();
	while (queued < total + (int64)frames * bufferCount) {
		memset(&info, 0, sizeof(info));
		info.info_size = sizeof(info);
		if (ioctl(fd, B_MULTI_BUFFER_EXCHANGE, &info, sizeof(info)) != 0) {
			fprintf(stderr, "hmulti_tone: BUFFER_EXCHANGE: %s\n",
				strerror(errno));
			break;
		}
		if (exchanges++ == 0) {
			firstTime = info.played_real_time;
			firstFrames = info.played_frames_count;
		}
		int32 next = (info.playback_buffer_cycle - 1 + bufferCount)
			% bufferCount;
		fill(next);
	}
	bigtime_t elapsed = info.played_real_time - firstTime;
	int64 played = info.played_frames_count - firstFrames;
	printf("played %" B_PRId64 " exchanges in %.2f s wall; driver clock: %"
		B_PRId64 " frames in %" B_PRId64 " us = %.1f Hz\n", exchanges,
		(system_time() - start) / 1e6, played, elapsed,
		elapsed > 0 ? played * 1e6 / elapsed : 0.0);

	ioctl(fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
	close(fd);
	return 0;
}
