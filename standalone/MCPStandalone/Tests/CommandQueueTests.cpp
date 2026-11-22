#include <string>

#include "CommandQueue.h"
#include "TestHarness.h"

TEST_CASE(CommandQueueTransfersRequestAndResponse) {
    CommandQueue queue;
    std::string detail;
    REQUIRE(queue.TryQueue(41, ControlCommand::ReloadConfig, detail));

    const auto request = queue.Claim();
    REQUIRE(request.has_value());
    REQUIRE_EQ(request->request_id, 41U);
    REQUIRE(request->command == ControlCommand::ReloadConfig);

    queue.Complete({0, "queued=1\n"});
    CommandResult result;
    REQUIRE(queue.Wait(41, 100, result) == QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 0U);
    REQUIRE_EQ(result.payload, "queued=1\n");
    REQUIRE(queue.Claim() == std::nullopt);
}

TEST_CASE(CommandQueueRejectsSecondPendingCommand) {
    CommandQueue queue;
    std::string detail;
    REQUIRE(queue.TryQueue(1, ControlCommand::ReloadConfig, detail));
    REQUIRE(!queue.TryQueue(2, ControlCommand::ReloadShaders, detail));
    REQUIRE(!detail.empty());
}

TEST_CASE(CommandQueueTimesOutAndAbandonsPendingRequest) {
    CommandQueue queue;
    std::string detail;
    REQUIRE(queue.TryQueue(7, ControlCommand::FrameAnalysis, detail));

    CommandResult result;
    REQUIRE(queue.Wait(7, 1, result) == QueueWaitResult::TimedOut);
    REQUIRE(!queue.Claim().has_value());

    REQUIRE(queue.TryQueue(8, ControlCommand::ReloadConfig, detail));
}

TEST_CASE(CommandQueueReleasesClaimedRequestAfterLateCompletion) {
    CommandQueue queue;
    std::string detail;
    REQUIRE(queue.TryQueue(9, ControlCommand::ReloadShaders, detail));
    REQUIRE(queue.Claim().has_value());

    CommandResult result;
    REQUIRE(queue.Wait(9, 1, result) == QueueWaitResult::TimedOut);
    queue.Complete({0, "late\n"});

    REQUIRE(queue.TryQueue(10, ControlCommand::ReloadConfig, detail));
}
