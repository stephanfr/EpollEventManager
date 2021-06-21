
#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>
#include <catch2/catch_all.hpp>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"

class EventFileDescriptor : public SEFUtility::EEM::EEMWorkerDispatchPrep
{
   public:
    EventFileDescriptor() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

    [[nodiscard]] uint64_t last_value() const { return last_value_; }
    [[nodiscard]] uint32_t num_callbacks() const { return num_callbacks_; }

    void send_event(uint64_t value) const
    {
        [[maybe_unused]] auto bytes_written = write(fd_, &value, sizeof(uint64_t));
        assert(bytes_written == sizeof(uint64_t));
    }

    explicit operator int() const { return fd_; }

    void event_callback(uint64_t value)
    {
        num_callbacks_++;
        last_value_ = value;
    }

    void prepare_worker_callback( [[maybe_unused]] int fd, SEFUtility::EEM::EEMCallbackQueue& queue) final
    {
        assert(fd == fd_);  //  NOLINT

        uint64_t value = 0;

        REQUIRE(read(fd_, static_cast<void*>(&value), sizeof(uint64_t)) == 8);

        queue.enqueue_callback(
            std::bind(&EventFileDescriptor::event_callback, this, value));  //  NOLINT(modernize-avoid-bind)
    }

   private:
    const int fd_;

    uint32_t num_callbacks_{0};
    uint64_t last_value_{0};
};
