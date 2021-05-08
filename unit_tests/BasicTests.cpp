
#include <spdlog/spdlog.h>

#include <catch2/catch_all.hpp>
#include <iostream>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"
#include "EpollEventManager.hpp"
#include "EventFileDescriptor.hpp"
#include "Result.hpp"

//  The pragma below is to disable to false errors flagged by intellisense for
//  Catch2 REQUIRE macros.

#if __INTELLISENSE__
#pragma diag_suppress 2486
#endif


TEST_CASE("Basic EpollEventManager Tests", "[basic]")
{
    spdlog::set_level(spdlog::level::debug);

    enum class EMMResultCodes
    {
        UNINITIALIZED = -1,
        SUCCESS = 0,
        FAILURE
    };

    class EEMResult : public SEFUtility::Result<EMMResultCodes>
    {
       public:
        EEMResult() : Result(Result::failure(EMMResultCodes::UNINITIALIZED, "")) {}
        EEMResult(const Result& result_to_copy) : Result(result_to_copy) {}
    };

    using EpollEventManagerBase = SEFUtility::EEM::EpollEventManager<EEMResult>;


    class EEMAddEventFDDirective : public SEFUtility::EEM::EEMDirective<EEMResult>
    {
       public:
        EEMAddEventFDDirective(EventFileDescriptor& efd) : efd_(efd) {}

        EEMResult handle_directive(SEFUtility::EEM::EEMFileDescriptorManager& fd_manager) final
        {
            REQUIRE( fd_manager.add_fd(efd_, efd_).success() );

            return EEMResult::success();
        }

       private:
        EventFileDescriptor& efd_;
    };
    

    class TestEventMgr : public EpollEventManagerBase
    {
       public:
        TestEventMgr() : EpollEventManagerBase(4, false, false) {}

        ~TestEventMgr() = default;
    };

    const struct timespec one_second
    {
        1, 0
    };

    TestEventMgr test_event_mgr;

    test_event_mgr.start_service_routine();

    EventFileDescriptor efd;

    EEMAddEventFDDirective add_efd_directive(efd);

    EEMResult result = test_event_mgr.send_directive(add_efd_directive);

    REQUIRE(result.succeeded());

    efd.send_event(123);

    clock_nanosleep(CLOCK_MONOTONIC, 0, &one_second, NULL);

    REQUIRE(efd.num_callbacks() == 1 );
    REQUIRE(efd.last_value() == 123);

    efd.send_event(456);

    clock_nanosleep(CLOCK_MONOTONIC, 0, &one_second, NULL);

    REQUIRE(efd.num_callbacks() == 2 );
    REQUIRE(efd.last_value() == 456);
}
