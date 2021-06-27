
#include <catch2/catch_all.hpp>
#include <iostream>

#include "EEMTestResult.hpp"
#include "SoftwareTimer.hpp"

#include "TimespecUtilities.hpp"

#if __INTELLISENSE__
#pragma diag_suppress 2486
#endif

using namespace SEFUtility::timespec;

typedef SEFUtility::EEM::EpollEventManager<EEMTestResult> EpollEventManagerImpl;
typedef SEFUtility::EEM::SoftwareTimer<EEMTestResult> SoftwareTimerImpl;

//  Simple callback function for the sw timer

long num_sw_timer_isr_calls = 0;

void sw_timer_isr_routine() { num_sw_timer_isr_calls++; }

//  Test cases follow

TEST_CASE("Test SW Timer", "[swtimer-base]")
{
    constexpr int MAX_NUMBER_OF_FDS = 24;
    constexpr unsigned long NUMBER_OF_REPETITIONS = 10;

    SECTION("SW Timer Basic Tests")
    {
        const struct timespec period = 1_s;

        const struct timespec timer_lifetime = ( period * NUMBER_OF_REPETITIONS ) + 1_s;
 
        EpollEventManagerImpl event_manager(MAX_NUMBER_OF_FDS, false, true);

        SoftwareTimerImpl sw_timer(event_manager, "Test SW Timer", period, true, NUMBER_OF_REPETITIONS,
                                   sw_timer_isr_routine, false);

        sw_timer.start();

        clock_nanosleep(CLOCK_MONOTONIC, 0, &timer_lifetime, NULL);

        REQUIRE(num_sw_timer_isr_calls == NUMBER_OF_REPETITIONS);
    }
}
