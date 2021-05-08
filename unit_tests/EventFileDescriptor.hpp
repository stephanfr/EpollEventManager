
#include <sys/eventfd.h>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"

class EventFileDescriptor : public SEFUtility::EEM::EEMWorkerDispatchPrep
{
   public:
    EventFileDescriptor() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)), num_callbacks_(0), last_value_(0) {}

    uint64_t    last_value() const { return last_value_; }
    uint32_t    num_callbacks() const { return num_callbacks_; }

    void send_event(uint64_t value) { write(fd_, &value, sizeof(uint64_t)); }

    operator int() const { return fd_; } 

    void    event_callback( uint64_t        value )
    {
        num_callbacks_++;
        last_value_ = value;
    }

    void prepare_worker_callback(int fd, SEFUtility::EEM::EEMCallbackQueue& queue)
    {
        assert( fd == fd_ );
        
        uint64_t value;

        REQUIRE( read(fd_, &value, sizeof(uint64_t) ) == 8 );

        queue.enqueue_callback(std::bind(&EventFileDescriptor::event_callback, this, value));
    }

   private:
    const int fd_;

    uint32_t    num_callbacks_;
    uint64_t    last_value_;
};
