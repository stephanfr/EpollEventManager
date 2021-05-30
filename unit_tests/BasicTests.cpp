
#include <spdlog/spdlog.h>

#include <catch2/catch_all.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <vector>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"
#include "EpollEventManager.hpp"
#include "EventFileDescriptor.hpp"
#include "MultithreadedTestFixture.hpp"
#include "Result.hpp"

//  The pragma below is to disable to false errors flagged by intellisense for
//  Catch2 REQUIRE macros.

#if __INTELLISENSE__
#pragma diag_suppress 2486
#endif

using namespace std::chrono_literals;

int64_t current_memory_consumption_in_kb()
{
    constexpr int MAX_CHAR_BUFFER_LENGTH = 129;

    std::ifstream file("/proc/self/status");
    std::string line;

    line.reserve(MAX_CHAR_BUFFER_LENGTH);

    int64_t mem_consumption = -1;

    while (!file.eof())
    {
        std::getline(file, line);

        if (line.find("VmSize:") != std::string::npos)
        {
            std::string kb_value = line.substr(line.find("VmSize:") + sizeof("VmSize:"));
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

class EEMTestResult
{
   public:
    EEMTestResult() : result_(SEFUtility::Result<EMMTestResultCodes>::failure(EMMTestResultCodes::UNINITIALIZED, "")) {}

    explicit EEMTestResult(const EEMTestResult& result_to_copy) : result_(result_to_copy.result_) {}

    EEMTestResult(EEMTestResult&& result_to_copy) : result_(std::move(result_to_copy.result_)) {}

    EEMTestResult& operator=(const EEMTestResult& result_to_copy)
    {
        result_ = result_to_copy.result_;

        return *this;
    }

    EEMTestResult& operator=(EEMTestResult&& result_to_move)
    {
        result_ = std::move(result_to_move.result_);

        return *this;
    }

    bool succeeded() { return result_.succeeded(); }
    bool failed() { return result_.failed(); }

    static EEMTestResult success() { return EEMTestResult(SEFUtility::Result<EMMTestResultCodes>::success()); }

    static EEMTestResult failure(EMMTestResultCodes error_code, const std::string& message)
    {
        return EEMTestResult(SEFUtility::Result<EMMTestResultCodes>::failure(error_code, message));
    }

   protected:
    SEFUtility::Result<EMMTestResultCodes> result_;

    EEMTestResult(SEFUtility::Result<EMMTestResultCodes> result) : result_(result) {}
};

using EpollEventManagerBase = SEFUtility::EEM::EpollEventManager<EEMTestResult>;

class EEMAddEventFDDirective : public SEFUtility::EEM::EEMDirective<EEMTestResult>
{
   public:
    explicit EEMAddEventFDDirective(EventFileDescriptor& efd) : efd_(efd) {}

    EEMTestResult handle_directive(SEFUtility::EEM::EEMFileDescriptorManager& fd_manager) final
    {
        SEFUtility::EEM::EEMResult result = fd_manager.add_fd(static_cast<int>(efd_), efd_);

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
    explicit EEMRemoveEventFDDirective(EventFileDescriptor& efd) : efd_(efd) {}

    EEMTestResult handle_directive(SEFUtility::EEM::EEMFileDescriptorManager& fd_manager) final
    {
        SEFUtility::EEM::EEMResult result = fd_manager.remove_fd(static_cast<int>(efd_));

        if (!result.succeeded())
        {
            return EEMTestResult::failure(EMMTestResultCodes::FAILURE, result.message());
        }

        return EEMTestResult::success();
    }

   private:
    EventFileDescriptor& efd_;
};

constexpr int MAX_NUMBER_OF_FDS = 24;

class TestEventMgr : public EpollEventManagerBase
{
   public:
    TestEventMgr() : EpollEventManagerBase(MAX_NUMBER_OF_FDS, false, false) {}
    TestEventMgr(const TestEventMgr&) = delete;
    TestEventMgr(TestEventMgr&&) = delete;

    const TestEventMgr& operator=(const TestEventMgr&) = delete;
    const TestEventMgr& operator=(TestEventMgr&&) = delete;

    ~TestEventMgr() override = default;
};

bool add_remove_send_event_main(TestEventMgr& test_event_mgr, long num_iterations)
{
    EventFileDescriptor efd;
    bool active = false;

    std::random_device random_dev;
    std::mt19937 mersenne_twist(random_dev());
    std::uniform_int_distribution<int> operation_random_distribution(1, 10);         //  NOLINT
    std::uniform_int_distribution<int> event_value_random_distribution(1, 1000000);  //  NOLINT

    for (long i = 0; i < num_iterations; i++)
    {
        int operation = operation_random_distribution(mersenne_twist);

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
            efd.send_event(event_value_random_distribution(mersenne_twist));
        }

        clock_nanosleep(CLOCK_MONOTONIC, 0, &two_hundred_microseconds, nullptr);
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

        std::random_device random_dev;
        std::mt19937 mersenne_twist(random_dev());
        std::uniform_int_distribution<int> event_value_random_distribution(1, 1000000);  //  NOLINT

        int value;  //  NOLINT(cppcoreguidelines-init-variables)

        for (int i = 0; i < 10; i++)  //  NOLINT
        {
            value = event_value_random_distribution(mersenne_twist);

            efd.send_event(value);

            clock_nanosleep(CLOCK_MONOTONIC, 0, &one_second, nullptr);

            REQUIRE(efd.num_callbacks() == i + 1);
            REQUIRE(efd.last_value() == value);
        }

        int final_value = value;

        EEMRemoveEventFDDirective remove_efd_directive(efd);

        result = test_event_mgr.send_directive(remove_efd_directive);

        REQUIRE(result.succeeded());

        for (int i = 0; i < 5; i++)  //  NOLINT
        {
            value = event_value_random_distribution(mersenne_twist);

            efd.send_event(value);

            clock_nanosleep(CLOCK_MONOTONIC, 0, &one_second, nullptr);

            REQUIRE(efd.num_callbacks() == 10);
            REQUIRE(efd.last_value() == final_value);
        }
    }

    SECTION("Thread Reentrancy", "[threading]")
    {
        spdlog::set_level(spdlog::level::critical);

        {
            TestEventMgr test_event_mgr;

            test_event_mgr.start_service_routine();

            SEFUtility::HeapWatcher::MultithreadedTestFixture test_fixture;

            SEFUtility::HeapWatcher::get_heap_watcher().start_watching();

            constexpr int NUMBER_OF_ITERATIONS = 100000;

            test_fixture.add_workload(
                20, std::bind(add_remove_send_event_main, std::ref(test_event_mgr), NUMBER_OF_ITERATIONS));

            test_fixture.start_workload();
            test_fixture.wait_for_completion();
        }

        std::this_thread::sleep_for(10s);

        auto snapshot = SEFUtility::HeapWatcher::get_heap_watcher().stop_watching();

        REQUIRE(snapshot.open_allocations().size() == 0);
    }
}
