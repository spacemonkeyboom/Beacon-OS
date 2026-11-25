/*
 * Copyright 2026, Beacon contributors.
 * Distributed under the terms of the MIT License.
 *
 * Beacon: remembers whether the user has been shown the time zone choice.
 * No lookup is made: the default zone is New York, and the user picks
 * theirs in Time preferences, opened once after the first network time
 * sync.
 */


#include "TimeZoneSetup.h"

#include <unistd.h>

#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


static const char* kMarkerName = "beacon_timezone_asked";


static bool
marker_path(BPath& path)
{
	return find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append(kMarkerName) == B_OK;
}


bool
time_zone_asked()
{
	BPath path;
	return !marker_path(path) || BEntry(path.Path()).Exists();
}


void
mark_time_zone_asked()
{
	BPath path;
	if (!marker_path(path))
		return;
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	file.Unset();
	sync();
}
