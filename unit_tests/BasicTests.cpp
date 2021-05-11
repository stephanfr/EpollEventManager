
#include <spdlog/spdlog.h>

#include <catch2/catch_all.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <vector>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"
#include "EpollEventManager.hpp"
#include "EventFileDescriptor.hpp"
#include "Result.hpp"

//  The pragma below is to disable to false errors flagged by intellisense for
//  Catch2 REQUIRE macros.

#if __INTELLISENSE__
#pragma diag_suppress 2486
#endif

using namespace std::chrono_literals;

long current_memory_consumption_in_kb()
{
    std::ifstream file("/proc/self/status");
    std::string line;

    line.reserve(129);

    long mem_consumption = -1;

    while (!file.eof())
    {
        std::getline(file, line);

        if (line.find("VmSize:") != std::string::npos)
        {
            std::string kb_value = line.substr(line.find("VmSize:") + 7);
            mem_consumption = stol(kb_value);
            break;
        }
    }

    file.close();

    return mem_consumption;
}

const struct timespec two_hundred_microseconds
{
    0, 200000
};

const struct timespec one_second
{
    1, 0
};

enum class EMMTestResultCodes
{
    UNINITIALIZED = -1,
    SUCCESS = 0,
    FAILURE
};

class EEMTestResult : public SEFUtility::Result<EMMTestResultCodes>
{
   public:
    EEMTestResult() : Result(Result::failure(EMMTestResultCodes::UNINITIALIZED, "")) {}
    EEMTestResult(const Result& result_to_copy) : Result(result_to_copy) {}
};

using EpollEventManagerBase = SEFUtility::EEM::EpollEventManager<EEMTestResult>;

class EEMAddEventFDDirective : public SEFUtility::EEM::EEMDirective<EEMTestResult>
{
   public:
    EEMAddEventFDDirective(EventFileDescriptor& efd) : efd_(efd) {}

    EEMTestResult handle_directive(SEFUtility::EEM::EEMFileDescriptorManager& fd_manager) final
    {
        SEFUtility::EEM::EEMResult result = fd_manager.add_fd(efd_, efd_);

        if (!result.succeeded())
        {
            return EEMTestResult::failure(EMMTestResultCodes::FAILURE, result.message());
        }

        return EEMTestResult::success();
    }

   private:
    EventFileDescriptor& efd_;
};

class EEMRemoveEventFDDirective : public SEFUtility::EEM::EEMDirective<EEMTestResult>
{
   public:
    EEMRemoveEventFDDirective(EventFileDescriptor& efd) : efd_(efd) {}

    EEMTestResult handle_directive(SEFUtility::EEM::EEMFileDescriptorManager& fd_manager) final
    {
        SEFUtility::EEM::EEMResult result = fd_manager.remove_fd(efd_);

        if (!result.succeeded())
        {
            return EEMTestResult::failure(EMMTestResultCodes::FAILURE, result.message());
        }

        return EEMTestResult::success();
    }

   private:
    EventFileDescriptor& efd_;
};

class TestEventMgr : public EpollEventManagerBase
{
   public:
    TestEventMgr() : EpollEventManagerBase(24, false, false) {}

    ~TestEventMgr() = default;
};

bool add_remove_send_event_main(TestEventMgr& test_event_mgr)
{
    EventFileDescriptor efd;
    bool active = false;

    for (int i = 0; i < 10000000; i++)
    {
        int operation = rand() % 10;

        if (operation == 0)
        {
            EEMAddEventFDDirective add_efd_directive(efd);

            EEMTestResult result = test_event_mgr.send_directive(add_efd_directive);

            if (result.succeeded())
            {
                active = true;
            }
        }
        else if (operation == 1)
        {
            EEMRemoveEventFDDirective remove_efd_directive(efd);

            EEMTestResult result = test_event_mgr.send_directive(remove_efd_directive);

            if (result.succeeded())
            {
                active = false;
            }
        }
        else
        {
            int value = rand();

            efd.send_event(value);
        }

        clock_nanosleep(CLOCK_MONOTONIC, 0, &two_hundred_microseconds, NULL);
    }

    return true;
};

TEST_CASE("Basic EpollEventManager Tests", "[basic]")
{
    SECTION("Basic Tests", "[basic]")
    {
        TestEventMgr test_event_mgr;

        test_event_mgr.start_service_routine();

        EventFileDescriptor efd;

        EEMAddEventFDDirective add_efd_directive(efd);

        EEMTestResult result = test_event_mgr.send_directive(add_efd_directive);

        REQUIRE(result.succeeded());

        int value;

        for (int i = 0; i < 10; i++)
        {
            value = rand();

            efd.send_event(value);

            clock_nanosleep(CLOCK_MONOTONIC, 0, &one_second, NULL);

            REQUIRE(efd.num_callbacks() == i + 1);
            REQUIRE(efd.last_value() == value);
        }

        int final_value = value;

        EEMRemoveEventFDDirective remove_efd_directive(efd);

        result = test_event_mgr.send_directive(remove_efd_directive);

        REQUIRE(result.succeeded());

        for (int i = 0; i < 5; i++)
        {
            value = rand();

            efd.send_event(value);

            clock_nanosleep(CLOCK_MONOTONIC, 0, &one_second, NULL);

            REQUIRE(efd.num_callbacks() == 10);
            REQUIRE(efd.last_value() == final_value);
        }
    }

    SECTION("Thread Reentrancy", "[threading]")
    {
        TestEventMgr test_event_mgr;

        test_event_mgr.start_service_routine();

        spdlog::set_level(spdlog::level::critical);

        std::cout << "Starting Process Memory Usage " << current_memory_consumption_in_kb() << std::endl;

        {
            std::vector<std::future<bool>> worker_threads;
            std::vector<long>               memory_size_readings;

            memory_size_readings.reserve(4096);

            for (int i = 0; i < 20; i++)
            {
                worker_threads.emplace_back(std::async(add_remove_send_event_main, std::ref(test_event_mgr)));
            }

            std::cout << "Process Memory Usage before loop " << current_memory_consumption_in_kb() << std::endl;

            for (int i = 0; i < 20; i++)
            {
                while (worker_threads[i].wait_for(30s) == std::future_status::timeout)
                {
                    memory_size_readings.push_back( current_memory_consumption_in_kb() );
                    std::cout << "Process Memory Usage in loop " << memory_size_readings.back() << std::endl;
                }

                worker_threads[i].get();
            }
        }

        std::cout << "Final Process Memory Usage " << current_memory_consumption_in_kb() << std::endl;
    }
}
