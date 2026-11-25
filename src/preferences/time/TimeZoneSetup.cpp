/*
 * Copyright 2026, Beacon contributors.
 * Distributed under the terms of the MIT License.
 *
 * Beacon: asks once, at the first login with a network, which time zone to
 * use. It suggests one looked up from the internet address (one HTTPS request
 * to ipapi.co) when the answer is a time zone Haiku knows, and always lets the
 * user keep the current zone or choose another in Time preferences.
 */


#include "TimeZoneSetup.h"

#include <stdio.h>
#include <string.h>

#include <Alert.h>
#include <Application.h>
#include <File.h>
#include <FindDirectory.h>
#include <LocaleRoster.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <TimeZone.h>

#include <MutableLocaleRoster.h>
#include <syscalls.h>


using BPrivate::MutableLocaleRoster;


static const char* kMarkerName = "beacon_timezone_asked";
static const char* kLookupCommand = "/bin/curl -fsS --max-time 10 "
	"-A Beacon-TimeZoneSetup https://ipapi.co/timezone 2>/dev/null";


static bool
lookup_time_zone(BString& id)
{
	// Wi-Fi and DNS can need a moment after "network available".
	for (int attempt = 0; attempt < 6; attempt++) {
		if (attempt > 0)
			snooze(10000000);

		FILE* pipe = popen(kLookupCommand, "r");
		if (pipe == NULL)
			continue;
		char buffer[128] = {};
		size_t length = fread(buffer, 1, sizeof(buffer) - 1, pipe);
		pclose(pipe);
		buffer[length] = '\0';

		id = buffer;
		id.Trim();
		// Only an "Area/City" name that Haiku knows counts as a detection.
		if (id.Length() < 3 || id.Length() > 64 || id.FindFirst('/') < 0
			|| id.FindFirst(' ') >= 0)
			continue;
		BTimeZone zone(id.String());
		if (zone.InitCheck() == B_OK && zone.ID() == id)
			return true;
	}
	id = "";
	return false;
}


static void
set_time_zone(const BTimeZone& zone)
{
	MutableLocaleRoster::Default()->SetDefaultTimeZone(zone);
	_kern_set_timezone(zone.OffsetFromGMT(), zone.ID().String(),
		zone.ID().Length());
}


static void
write_marker()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(kMarkerName) != B_OK)
		return;
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
}


int
run_time_zone_setup()
{
	BPath marker;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &marker) == B_OK
		&& marker.Append(kMarkerName) == B_OK
		&& BEntry(marker.Path()).Exists())
		return 0;

	BApplication app("application/x-vnd.Beacon-TimeZoneSetup");

	BTimeZone current;
	BLocaleRoster::Default()->GetDefaultTimeZone(&current);

	BString detected;
	bool found = lookup_time_zone(detected);

	BString text("Set your time zone\n\n");
	BAlert* alert;
	if (found && detected != current.ID()) {
		BTimeZone zone(detected.String());
		text << "Beacon suggests " << zone.Name() << " (" << detected
			<< ").\n\nThis suggestion comes from your internet address, "
			"looked up once with ipapi.co. Your time zone is now "
			<< current.ID() << ".\n\nYou can change it any time in "
			"Preferences " B_UTF8_ELLIPSIS " Time.";
		BString use("Use ");
		use << zone.Name();
		BString keep("Keep ");
		keep << current.ID();
		alert = new BAlert("Time zone", text.String(), "Choose another"
			B_UTF8_ELLIPSIS, keep.String(), use.String(), B_WIDTH_AS_USUAL,
			B_IDEA_ALERT);
		alert->SetShortcut(1, B_ESCAPE);
		int32 choice = alert->Go();
		if (choice == 2)
			set_time_zone(zone);
		else if (choice == 0)
			be_roster->Launch("application/x-vnd.Haiku-Time");
	} else {
		if (found) {
			text << "Your time zone is " << current.ID() << ", which matches "
				"your internet address (looked up once with ipapi.co).";
		} else {
			text << "Beacon couldn't detect your time zone, so it is set to "
				<< current.ID() << ".";
		}
		text << "\n\nYou can change it any time in Preferences "
			B_UTF8_ELLIPSIS " Time.";
		alert = new BAlert("Time zone", text.String(), "Choose another"
			B_UTF8_ELLIPSIS, "OK", NULL, B_WIDTH_AS_USUAL, B_IDEA_ALERT);
		alert->SetShortcut(1, B_ESCAPE);
		if (alert->Go() == 0)
			be_roster->Launch("application/x-vnd.Haiku-Time");
	}

	write_marker();
	return 0;
}
