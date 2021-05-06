#include <spdlog/spdlog.h>
#include <catch2/catch_all.hpp>

#include "EpollEventManager.hpp"


//  The pragma below is to disable to false errors flagged by intellisense for Catch2 REQUIRE macros.

#if __INTELLISENSE__
#pragma diag_suppress 2486
#endif

TEST_CASE("Basic EpollEventManager Tests", "[basic]")
{
    spdlog::set_level(spdlog::level::debug);
}
