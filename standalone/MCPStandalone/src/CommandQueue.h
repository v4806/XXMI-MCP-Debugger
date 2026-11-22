#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "AdapterTypes.h"

struct QueuedCommand {
    std::uint32_t request_id = 0;
    ControlCommand command = ControlCommand::ReloadConfig;
};

enum class QueueWaitResult {
    Completed,
    TimedOut,
    InvalidRequest,
};

class CommandQueue {
public:
    bool TryQueue(
        std::uint32_t request_id,
        ControlCommand command,
        std::string& detail);
    std::optional<QueuedCommand> Claim();
    void Complete(const CommandResult& result);
    QueueWaitResult Wait(
        std::uint32_t request_id,
        std::uint32_t timeout_ms,
        CommandResult& result);

private:
    enum class State {
        Empty,
        Pending,
        Claimed,
        Abandoned,
        ResponseReady,
    };

    std::mutex mutex_;
    std::condition_variable condition_;
    State state_ = State::Empty;
    QueuedCommand request_;
    CommandResult response_;
    bool response_abandoned_ = false;
};
