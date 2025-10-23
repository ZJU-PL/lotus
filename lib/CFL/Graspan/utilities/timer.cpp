#include "CFL/Graspan/utilities/timer.h"


Timer::Timer() {}

unsigned long long Timer::getCurrTime()
{
	struct timeval tv;
	gettimeofday(&tv, 0);

	return (unsigned long long)(tv.tv_sec) * 1000 + (unsigned long long)(tv.tv_usec) / 1000;
}

void Timer::startTimer() { start = getCurrTime(); }
void Timer::endTimer() { end = getCurrTime(); }

unsigned long long Timer::totalTime() { return (end - start); }


std::string Timer::hmsFormat()
    {
        int seconds, minutes, hours;
        int x = totalTime() / 1000;
        seconds = x % 60;
        x /= 60;
        minutes = x % 60;
        x /= 60;
        hours = x;

        std::stringstream output;
        output << hours << " h, " << minutes << " m, " << seconds << " s";

        return output.str();
    }

std::string Timer::timeInSecs()
    {
        std::stringstream output;
        output << totalTime() / 1000 << " s";

        return output.str();
    }

std::string Timer::toString()
    {
        std::stringstream output;
        output << totalTime() << " ms";

        return output.str();
    }
