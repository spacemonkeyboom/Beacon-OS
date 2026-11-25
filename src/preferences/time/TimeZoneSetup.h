/*
 * Copyright 2026, Beacon contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef TIME_ZONE_SETUP_H
#define TIME_ZONE_SETUP_H


// Beacon: the first time the network comes up, Time --boot-update sets the
// clock (New York is the default zone) and then opens Time preferences on
// the Time zone tab, once.
bool time_zone_asked();
void mark_time_zone_asked();


#endif	// TIME_ZONE_SETUP_H
