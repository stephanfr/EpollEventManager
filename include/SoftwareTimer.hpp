#pragma once

#include <sys/timerfd.h>
#include <time.h>

#include <atomic>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"
#include "EpollEventManager.hpp"

namespace SEFUtility::EEM
{
    class SoftwareTimerCallback
    {
       public:
        virtual void on_expiration(void) = 0;
    };

    template <typename R, typename Ex = std::runtime_error, class... Bases>
    class SoftwareTimer : public EEMWorkerDispatchPrep, public Bases...
    {
       public:
        SoftwareTimer(EpollEventManager<R, Ex>& event_manager, const std::string name, struct timespec period,
                      bool repeating, uint32_t num_repetitions_requested, SoftwareTimerCallback& expiration_callback,
                      bool autostart)
            : SoftwareTimer(event_manager, name, period, repeating, num_repetitions_requested,
                            std::bind(&SoftwareTimerCallback::on_expiration, &expiration_callback), autostart)
        {
        }

        SoftwareTimer(EpollEventManager<R, Ex>& event_manager, const std::string name, struct timespec period,
                      bool repeating, uint32_t num_repetitions_requested, std::function<void()> expiration_callback,
                      bool autostart)
            : name_(name),
              event_manager_(event_manager),
              expiration_callback_(expiration_callback),
              running_(false),
              period_(period),
              repeating_(repeating),
              num_repetitions_requested_(num_repetitions_requested),
              current_num_repetitions_(0),
              fd_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC))
        {
            if (fd_ < 0)
            {
                std::string message("Unable to add descriptor to epoll_ctl in EpollEventManager::add_fd().  errno = ");
                message += strerror(errno);

                throw Ex(message);
            }

            if (autostart)
            {
                auto start_result = start();

                if (!start_result.succeeded())
                {
                    throw Ex(start_result.message());
                }
            }
        }

        SoftwareTimer( const SoftwareTimer& ) = delete;
        SoftwareTimer( const SoftwareTimer&& ) = delete;

        ~SoftwareTimer() { stop(); }

        SoftwareTimer&  operator=( const SoftwareTimer& ) = delete;
        SoftwareTimer&  operator=( const SoftwareTimer&& ) = delete;


        [[nodiscard]] int fd() const { return fd_; }

        [[nodiscard]] const std::string& name() const { return name_; }

        [[nodiscard]] bool running() const { return running_; }

        [[nodiscard]] bool repeating() const { return repeating_; }

        [[nodiscard]] const struct timespec& period() const { return period_; }

        R start()
        {
            if (!running_)
            {
                StartSoftwareTimerDirective start_directive(*this);

                event_manager_.send_directive(start_directive);
            }

            return R::success();
        }

        R stop()
        {
            if (running_)
            {
                StopSoftwareTimerDirective stop_directive(*this);

                event_manager_.send_directive(stop_directive);
            }

            return R::success();
        }

        void prepare_worker_callback([[maybe_unused]] int fd, SEFUtility::EEM::EEMCallbackQueue& queue)

        {
            uint64_t num_expirations;

            int bytes_read = read(this->fd(), &num_expirations, sizeof(num_expirations));

            if (bytes_read < 0)
            {
                SPDLOG_ERROR("Error reading from timerfd file descriptor.  errno = {}", errno);
                return;
            }

            for (int i = 0; (i < num_expirations) && (current_num_repetitions_ < num_repetitions_requested_); i++)
            {
                queue.enqueue_callback(expiration_callback_);
                current_num_repetitions_++;
            }

            if (current_num_repetitions_ >= num_repetitions_requested_)
            {
                stop_internal();
            }
        }

       private:
        EpollEventManager<R, Ex>& event_manager_;

        const std::string name_;
        const struct timespec period_;
        const bool repeating_;
        const uint32_t num_repetitions_requested_;
        std::function<void()> expiration_callback_;

        int fd_;
        std::atomic_bool running_;
        std::atomic<uint32_t> current_num_repetitions_;

        R start_internal()
        {
            struct itimerspec timer_spec;
            const struct timespec zero_timespec = {0, 0};

            timer_spec.it_interval = repeating_ ? period_ : zero_timespec;
            timer_spec.it_value = period_;

            event_manager_.add_fd(fd_, *this);

            timerfd_settime(fd_, 0, &timer_spec, NULL);

            running_ = true;

            return R::success();
        }

        R stop_internal()
        {
            struct itimerspec timer_spec;
            const struct timespec zero_timespec = {0, 0};

            timer_spec.it_interval = zero_timespec;
            timer_spec.it_value = zero_timespec;

            timerfd_settime(fd_, 0, &timer_spec, NULL);

            event_manager_.remove_fd(fd_);

            running_ = false;

            return R::success();
        }

        friend class StartSoftwareTimerDirective;
        friend class StopSoftwareTimerDirective;

        class StartSoftwareTimerDirective : public EEMDirective<R>
        {
           public:
            StartSoftwareTimerDirective(SoftwareTimer& timer) : timer_(timer) {}

            R handle_directive(EEMFileDescriptorManager& fd_manager)
            {
                SPDLOG_TRACE("Entering StartSoftwareTimerDirective::handle_directive() for SoftwareTimer {}.",
                             timer_.name());

                timer_.start_internal();

                return R::success();
            }

           private:
            SoftwareTimer& timer_;
        };

        class StopSoftwareTimerDirective : public EEMDirective<R>
        {
           public:
            StopSoftwareTimerDirective(SoftwareTimer& timer) : timer_(timer) {}

            R handle_directive(EEMFileDescriptorManager& fd_manager)
            {
                SPDLOG_TRACE("Entering StopSoftwareTimerDirective::handle_directive() for SoftwareTimer {}.",
                             timer_.name());

                timer_.stop_internal();

                return R::success();
            }

           private:
            SoftwareTimer& timer_;
        };
    };

}  // namespace SEFUtility::EEM
