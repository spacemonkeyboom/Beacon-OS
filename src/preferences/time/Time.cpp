/*
 * Copyright 2002-2021, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Andrew McCall <mccall@digitalparadise.co.uk>
 *		Mike Berg <mike@berg-net.us>
 *		Julun <host.haiku@gmx.de>
 *		Hamish Morrison <hamish@lavabit.com>
 *		Panagiotis "Ivory" Vasilopoulos <git@n0toose.net>
 */


#include "Time.h"

#include <locale.h>
#include <stdio.h>
#include <unistd.h>

#include <AboutWindow.h>
#include <Alert.h>
#include <Catalog.h>
#include <Locale.h>
#include <LocaleRoster.h>

#include "NetworkTimeView.h"
#include "TimeMessages.h"
#include "TimeWindow.h"
#include "TimeZoneSetup.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Time"


const char* kAppSignature = "application/x-vnd.Haiku-Time";


TimeApplication::TimeApplication()
	:
	BApplication(kAppSignature),
	fWindow(NULL)
{
	fWindow = new TTimeWindow();
}


TimeApplication::~TimeApplication()
{
}


void
TimeApplication::SelectTimeZoneTab()
{
	fWindow->PostMessage(kSelectTimeZoneTab);
}


void
TimeApplication::ReadyToRun()
{
	fWindow->Show();
}


void
TimeApplication::AboutRequested()
{
	BAboutWindow* window = new BAboutWindow(B_TRANSLATE_SYSTEM_NAME(
		"Time & Date"), kAppSignature);

	const char* authors[] = {
		"Mike Berg",
		"Andrew Edward McCall",
		"Hamish Morrison",
		"Philippe Saint-Pierre",
		"Panagiotis \"Ivory\" Vasilopoulos",
		"Julun",
		NULL
	};

	window->AddCopyright(2021, "Haiku, Inc.");
	window->AddAuthors(authors);

	window->Show();
}


void
TimeApplication::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kSelectClockTab:
		case kShowHideTime:
		case B_LOCALE_CHANGED:
			fWindow->PostMessage(message);
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
}


// Beacon: one NTP attempt with a time limit. The attempt runs in its own
// thread; if it hangs (blocked DNS lookup or socket), it is abandoned and
// left behind; the process exits when the boot job ends.
struct TimedUpdate {
	Settings*	settings;
	const char*	errorString;
	int32		errorCode;
	status_t	status;
};


static status_t
timed_update_thread(void* data)
{
	TimedUpdate* update = (TimedUpdate*)data;
	update->status = update_time(*update->settings, &update->errorString,
		&update->errorCode);
	return B_OK;
}


static status_t
timed_update_time(Settings& settings, bigtime_t timeout,
	const char** _errorString, int32* _errorCode)
{
	// Leaked on purpose if the thread hangs: it may still write to it.
	TimedUpdate* update = new TimedUpdate{&settings, NULL, 0, B_ERROR};
	thread_id thread = spawn_thread(timed_update_thread, "ntp attempt",
		B_NORMAL_PRIORITY, update);
	if (thread < 0) {
		delete update;
		return update_time(settings, _errorString, _errorCode);
	}
	resume_thread(thread);
	status_t exitValue;
	status_t result = wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, timeout,
		&exitValue);
	if (result != B_OK) {
		*_errorString = "NTP attempt timed out";
		*_errorCode = 0;
		return B_TIMED_OUT;
	}
	*_errorString = update->errorString;
	*_errorCode = update->errorCode;
	status_t status = update->status;
	delete update;
	return status;
}


int
main(int argc, char** argv)
{
	if (argc > 1) {
		bool bootUpdate = strcmp(argv[1], "--boot-update") == 0;
		if (strcmp(argv[1], "--update") != 0 && !bootUpdate) {
			fprintf(stderr, "Usage: %s [--update]\n", argv[0]);
			fprintf(stderr, "    --update    Optionally force an NTP clock sync and exit\n\n");
			return 0;
		}

		Settings settings;
		// Beacon: the boot job runs by default (the board has no
		// battery-backed clock); skip it only if the user turned it off.
		if (bootUpdate && !settings.GetSynchronizeAtBoot()
			&& time_zone_asked())
			return 0;

		// Sync unless only the first time zone choice is left to do.
		bool doSync = !bootUpdate || settings.GetSynchronizeAtBoot();
		const char* errorString = NULL;
		int32 errorCode = 0;
		status_t status = B_OK;
		if (doSync && !bootUpdate)
			status = update_time(settings, &errorString, &errorCode);
		// Beacon: at boot, each attempt runs in its own thread and is given
		// up after 15 s (a DNS lookup on a lossy Wi-Fi link once blocked the
		// job for over half an hour). Retry quickly at first: Wi-Fi and DNS
		// may need a moment after "network available".
		for (int32 attempt = 0; doSync && bootUpdate && attempt < 20;
				attempt++) {
			if (attempt > 0)
				snooze(attempt < 5 ? 2000000 : 10000000);
			status = timed_update_time(settings, 15000000, &errorString,
				&errorCode);
			if (status == B_OK)
				break;
		}
		if (!doSync) {
			// nothing to report
		} else if (status == B_OK) {
			printf("Synchronization successful\n");
		} else if (errorCode != 0) {
			printf("The following error occurred "
					"while synchronizing:\n%s: %s\n",
				errorString, strerror(errorCode));
		} else {
			printf("The following error occurred while synchronizing:\n%s\n",
				errorString);
		}

		// Beacon: after the first network time sync (New York by default),
		// open Time preferences on the Time zone tab, once.
		if (bootUpdate && !time_zone_asked()) {
			mark_time_zone_asked();
			setlocale(LC_ALL, "");
			TimeApplication app;
			app.SelectTimeZoneTab();
			app.Run();
		}
	} else {
		setlocale(LC_ALL, "");

		TimeApplication app;
		setuid(0);
		app.Run();
	}

	return 0;
}

