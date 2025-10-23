#ifndef TIMER_H
#define TIMER_H

#include <sys/time.h>

#include <time.h>
#include <iostream>
#include <sstream>
#include <string>

class Timer
{
	unsigned long long start;
	unsigned long long end;

	// gets the current time in milliseconds
	unsigned long long getCurrTime();

	public:
	Timer();

	void startTimer();
	void endTimer();

	// return the amount of milliseconds the timer ran for
	unsigned long long totalTime();

	// return a string of the total time in (hours, minutes, seconds) format
	// "[HOURS] h, [MINUTES] m, [SECONDS] s"
	std::string hmsFormat();

	// return a string of the total time in seconds
	std::string timeInSecs();

	// return a string of the time in milliseconds "[MILISECS] ms"
	std::string toString();
};


#endif
