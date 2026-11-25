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
		if (bootUpdate && !settings.GetSynchronizeAtBoot())
			return 0;

		const char* errorString = NULL;
		int32 errorCode = 0;
		status_t status = update_time(settings, &errorString, &errorCode);
		// Wi-Fi and DNS may need a moment after "network available".
		for (int32 attempt = 1; bootUpdate && status != B_OK && attempt < 12;
				attempt++) {
			snooze(10000000);
			errorString = NULL;
			errorCode = 0;
			status = update_time(settings, &errorString, &errorCode);
		}
		if (status == B_OK) {
			printf("Synchronization successful\n");
		} else if (errorCode != 0) {
			printf("The following error occurred "
					"while synchronizing:\n%s: %s\n",
				errorString, strerror(errorCode));
		} else {
			printf("The following error occurred while synchronizing:\n%s\n",
				errorString);
		}
	} else {
		setlocale(LC_ALL, "");

		TimeApplication app;
		setuid(0);
		app.Run();
	}

	return 0;
}

