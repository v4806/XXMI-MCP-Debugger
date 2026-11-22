#include "CommandQueue.h"

#include <chrono>

bool CommandQueue::TryQueue(
    std::uint32_t request_id,
    ControlCommand command,
    std::string& detail) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Empty) {
        detail = state_ == State::Abandoned
                     ? "previous command is abandoned and awaiting render cleanup"
                     : "another native command is pending";
        return false;
    }
    request_ = {request_id, command};
    response_abandoned_ = false;
    response_ = {};
    state_ = State::Pending;
    detail = "command queued";
    return true;
}

std::optional<QueuedCommand> CommandQueue::Claim() {
    std::lock_guard lock(mutex_);
    if (state_ == State::Abandoned) {
        state_ = State::Empty;
        return std::nullopt;
    }
    if (state_ != State::Pending)
        return std::nullopt;
    state_ = State::Claimed;
    return request_;
}

void CommandQueue::Complete(const CommandResult& result) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Claimed)
            return;
        if (response_abandoned_) {
            state_ = State::Empty;
            request_ = {};
            response_ = {};
            response_abandoned_ = false;
            return;
        }
        response_ = result;
        state_ = State::ResponseReady;
    }
    condition_.notify_all();
}

QueueWaitResult CommandQueue::Wait(
    std::uint32_t request_id,
    std::uint32_t timeout_ms,
    CommandResult& result) {
    std::unique_lock lock(mutex_);
    if (request_.request_id != request_id)
        return QueueWaitResult::InvalidRequest;

    const auto ready = condition_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [this, request_id] {
            return request_.request_id == request_id &&
                   state_ == State::ResponseReady;
        });
    if (ready) {
        result = response_;
        state_ = State::Empty;
        request_ = {};
        response_abandoned_ = false;
        return QueueWaitResult::Completed;
    }

    if (state_ == State::ResponseReady &&
        request_.request_id == request_id) {
        result = response_;
        state_ = State::Empty;
        request_ = {};
        response_abandoned_ = false;
        return QueueWaitResult::Completed;
    }
    if (state_ == State::Pending) {
        state_ = State::Abandoned;
    } else if (state_ == State::Claimed) {
        response_abandoned_ = true;
    }
    return QueueWaitResult::TimedOut;
}
