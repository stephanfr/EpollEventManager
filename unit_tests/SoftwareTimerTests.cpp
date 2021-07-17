
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

long num_sw_timer_expirations = 0;

void sw_timer_expiration_routine(struct timespec timestamp) { num_sw_timer_expirations++; }

class TestTimerCallback : public SEFUtility::EEM::SoftwareTimerCallback
{
   public:
    void on_expiration(struct timespec timestamp) final { num_callbacks_++; }

    long num_callbacks() const { return (num_callbacks_); }

   private:
    long num_callbacks_ = 0;
};

//  Test cases follow

TEST_CASE("Test SW Timer", "[swtimer-base]")
{
    constexpr int MAX_NUMBER_OF_FDS = 24;
    constexpr unsigned long NUMBER_OF_REPETITIONS = 10;

    constexpr int NUM_TIMERS_IN_MT_TEST = 20;

    SECTION("SW Timer Basic Tests")
    {
        const struct timespec period = 1_s;

        const struct timespec timer_lifetime = (period * NUMBER_OF_REPETITIONS) + 1_s;

        EpollEventManagerImpl event_manager(MAX_NUMBER_OF_FDS, false, true);

        SoftwareTimerImpl sw_timer(event_manager, "Test SW Timer", period, true, NUMBER_OF_REPETITIONS,
                                   sw_timer_expiration_routine, false);

        sw_timer.start();

        clock_nanosleep(CLOCK_MONOTONIC, 0, &timer_lifetime, NULL);

        REQUIRE(!sw_timer.running());
        REQUIRE(num_sw_timer_expirations == NUMBER_OF_REPETITIONS);
    }

    SECTION("SW Timer Multithreaded Tests")
    {
        EpollEventManagerImpl event_manager(MAX_NUMBER_OF_FDS, false, true);

        std::random_device random_dev;
        std::mt19937 mersenne_twist(random_dev());
        std::uniform_int_distribution<int> num_repetitions_distribution(1, 200);         //  NOLINT
        std::uniform_int_distribution<int> period_in_ms_distribution(1, 100);  //  NOLINT

        unsigned long    max_timer_wait = 0;

        std::array<TestTimerCallback, NUM_TIMERS_IN_MT_TEST> callbacks;
        std::array<long, NUM_TIMERS_IN_MT_TEST> num_expected_callbacks;

        std::vector<std::unique_ptr<SoftwareTimerImpl>> timers;

        for (int i = 0; i < NUM_TIMERS_IN_MT_TEST; i++)
        {
            long repetitions = num_repetitions_distribution(mersenne_twist);
            unsigned long period_in_ms = period_in_ms_distribution(mersenne_twist);

            const struct timespec period = 1_ms * period_in_ms;

            timers.emplace_back(new SoftwareTimerImpl(event_manager, "Test SW Timer", period, true,
                                                      repetitions, callbacks[i], false));

            num_expected_callbacks[i] = repetitions;

            max_timer_wait = std::max( max_timer_wait, repetitions * period_in_ms );
        }

        for (int i = 0; i < NUM_TIMERS_IN_MT_TEST; i++)
        {
            timers[i]->start();
        }

        const timespec timer_lifetime = ( 1_ms * max_timer_wait ) + 10_s;

        std::cout << max_timer_wait << std::endl;

        clock_nanosleep(CLOCK_MONOTONIC, 0, &timer_lifetime, NULL);

        for (int i = 0; i < NUM_TIMERS_IN_MT_TEST; i++)
        {
            REQUIRE(!timers[i]->running());
            REQUIRE(callbacks[i].num_callbacks() == num_expected_callbacks[i]);
        }
    }
}
