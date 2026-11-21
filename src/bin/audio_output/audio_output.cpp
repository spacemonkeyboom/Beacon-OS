/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * audio_output: show or switch the output of a multi_audio (hmulti) device
 * that has an "Output" selector (B_MULTI_MIX_MUX), like the JH7110 audio
 * driver (Headphones / HDMI).
 *
 *   audio_output [-d device]            print the current output and choices
 *   audio_output [-d device] hdmi       switch to HDMI
 *   audio_output [-d device] jack       switch to the headphone jack
 *
 * Works while the media server plays: the driver lets any number of
 * programs open the device for the description and mix ioctls; only the
 * streaming ioctls belong to one opener. The driver saves the choice in
 * ~/config/settings/kernel/drivers/pwmdac_jh7110 ("output hdmi|headphones").
 */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <OS.h>

#include <hmulti_audio.h>


static const char* kDefaultDevice = "/dev/audio/hmulti/pwmdac/1";
static const int32 kMaxControls = 128;


static void
usage()
{
	fprintf(stderr, "usage: audio_output [-d device] [hdmi|jack|headphones]\n"
		"default device: %s\n", kDefaultDevice);
	exit(1);
}


static bool
name_matches(const char* name, const char* wanted)
{
	// "jack" and "headphones" both mean the headphone output; the driver
	// may append " (unavailable)".
	if (strcasecmp(wanted, "jack") == 0 || strcasecmp(wanted, "headphone") == 0)
		wanted = "headphones";
	size_t length = strlen(wanted);
	return strncasecmp(name, wanted, length) == 0
		&& (name[length] == '\0' || name[length] == ' ');
}


int
main(int argc, char** argv)
{
	const char* path = kDefaultDevice;
	int opt;
	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		switch (opt) {
			case 'd': path = optarg; break;
			default: usage();
		}
	}
	if (argc - optind > 1)
		usage();
	const char* wanted = optind < argc ? argv[optind] : NULL;

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "audio_output: cannot open %s: %s\n", path,
			strerror(errno));
		return 1;
	}

	multi_mix_control* controls = (multi_mix_control*)calloc(kMaxControls,
		sizeof(multi_mix_control));
	if (controls == NULL)
		return 1;
	multi_mix_control_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	info.control_count = kMaxControls;
	info.controls = controls;
	if (ioctl(fd, B_MULTI_LIST_MIX_CONTROLS, &info, sizeof(info)) != 0) {
		fprintf(stderr, "audio_output: LIST_MIX_CONTROLS: %s\n",
			strerror(errno));
		return 1;
	}

	// The first MUX control called "Output" and its values, in order.
	int32 mux = -1;
	for (int32 i = 0; i < info.control_count; i++) {
		if ((controls[i].flags & B_MULTI_MIX_KIND_MASK) == B_MULTI_MIX_MUX
			&& strcasecmp(controls[i].name, "Output") == 0) {
			mux = i;
			break;
		}
	}
	if (mux < 0) {
		fprintf(stderr, "audio_output: %s has no \"Output\" selector\n", path);
		return 1;
	}
	int32 values[16];
	int32 valueCount = 0;
	for (int32 i = 0; i < info.control_count && valueCount < 16; i++) {
		if ((controls[i].flags & B_MULTI_MIX_KIND_MASK) == B_MULTI_MIX_MUX_VALUE
			&& controls[i].parent == controls[mux].id)
			values[valueCount++] = i;
	}

	multi_mix_value value;
	multi_mix_value_info valueInfo;
	memset(&value, 0, sizeof(value));
	memset(&valueInfo, 0, sizeof(valueInfo));
	value.id = controls[mux].id;
	valueInfo.values = &value;
	valueInfo.item_count = 1;

	if (wanted != NULL) {
		int32 index = -1;
		for (int32 v = 0; v < valueCount; v++) {
			if (name_matches(controls[values[v]].name, wanted)) {
				index = v;
				break;
			}
		}
		if (index < 0) {
			fprintf(stderr, "audio_output: no output called \"%s\"\n", wanted);
			return 1;
		}
		value.mux = index;
		if (ioctl(fd, B_MULTI_SET_MIX, &valueInfo, sizeof(valueInfo)) != 0) {
			fprintf(stderr, "audio_output: switching to %s failed: %s\n",
				controls[values[index]].name, strerror(errno));
			return 1;
		}
		memset(&value, 0, sizeof(value));
		value.id = controls[mux].id;
	}

	if (ioctl(fd, B_MULTI_GET_MIX, &valueInfo, sizeof(valueInfo)) != 0) {
		fprintf(stderr, "audio_output: GET_MIX: %s\n", strerror(errno));
		return 1;
	}
	uint32 current = value.mux & 0xff;
	printf("output: %s\n", current < (uint32)valueCount
		? controls[values[current]].name : "?");
	printf("choices:");
	for (int32 v = 0; v < valueCount; v++) {
		printf(" %s%s%s", v == (int32)current ? "[" : "",
			controls[values[v]].name, v == (int32)current ? "]" : "");
		if (v + 1 < valueCount)
			printf(",");
	}
	printf("\n");

	free(controls);
	close(fd);
	return 0;
}
