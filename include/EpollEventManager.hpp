#pragma once

#include <blockingconcurrentqueue.h>
#include <concurrentqueue.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <atomic>
#include <functional>
#include <future>
#include <map>

#include "EEMDirectiveAndWorkerDispatchPrep.hpp"
#include "spdlog/spdlog.h"

namespace SEFUtility::EEM
{
    //
    //  R is the result class for any dispatch results.
    //  Ex is the Exception class which defaults to std::runtime_error
    //

    template <typename R, typename Ex = std::runtime_error>
    class EpollEventManager : public EEMFileDescriptorManager, public EEMCallbackQueue
    {
       public:
        //  EpollEventManager provides a framework for responsing to activity on file
        //  descriptors using
        //      epoll with the ability to offload callbacks and pass control
        //      instructions to the service routine.

        explicit EpollEventManager(uint32_t max_number_of_fds, bool realtime_scheduling = false,
                                   bool autostart_service_routine = true)
            : max_number_of_fds_(max_number_of_fds),
              realtime_scheduling_(realtime_scheduling),
              service_routine_running_(false),
              worker_routine_running_(false),
              num_events_dispatched_(0),
              epoll_wait_timeout_in_ms_(TEN_SECONDS_IN_MILLISECONDS),
              event_fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
              epoll_fd_(epoll_create1(EPOLL_CLOEXEC))
        {
            //  Add the event file descriptor so that we can gracefully interrupt the
            //  epoll_wait.  Fail immediately, if for some reason it cannot be added.

            struct epoll_event epoll_control_event
            {
            };

            epoll_control_event.events = EPOLLIN;
            epoll_control_event.data.fd = event_fd_;

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &epoll_control_event) < 0)
            {
                SPDLOG_CRITICAL(
                    "Unable to add event fd to epoll_ctl in "
                    "ISRHandler::interrupt_service_routine().  errno = {}",
                    errno);
                throw Ex(
                    "Unable to add event fd to epoll_ctl in "
                    "ISRHandler::interrupt_service_routine()");
            }

            //  Autostart the service routine if requested

            if (autostart_service_routine)
            {
                start_service_routine();
            }
        }

        EpollEventManager(const EpollEventManager&) = delete;
        EpollEventManager(EpollEventManager&&) = delete;

        EpollEventManager& operator=(const EpollEventManager&) = delete;
        EpollEventManager& operator=(EpollEventManager&&) = delete;

        virtual ~EpollEventManager()
        {
            //  Shutdown the service routine if it is still running

            shutdown_service_routine();

            //  Close the file descriptors

            close(event_fd_);
            close(epoll_fd_);
        }

        [[nodiscard]] uint64_t num_events_dispatched() const { return num_events_dispatched_; }

        void start_service_routine()
        {
            //  Return immediately doing nothing if the epoll service thread is already
            //  running

            if (epoll_service_thread_.joinable() || service_routine_running_)
            {
                return;
            }

            //  Create the thread and let it start.

            epoll_service_thread_ = std::thread(&EpollEventManager<R, Ex>::epoll_service_routine, this);

            //  Wait for the service thread to be running before we return

            while (!service_routine_running_)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        void shutdown_service_routine()
        {
            //  If the thread is not joinable, return immediately doing nothing

            if (!epoll_service_thread_.joinable())
            {
                return;
            }

            //  Set the service routine runnign falg false and trigger the event file
            //  descriptor

            service_routine_running_ = false;

            trigger_event_fd();

            //  Wait for the service thread to join

            epoll_service_thread_.join();
        }

        template <typename D>
        R send_directive(D& directive)
        {
            static_assert(std::is_convertible<D*, EEMDirective<R>*>::value,
                          "EpollEventManager::send_directive template argument 'D' "
                          "must be covertible to EEMDirective");

            std::promise<R> result_promise;
            std::future<R> result_future = result_promise.get_future();

            isr_directives_.enqueue(std::make_pair(&directive, &result_promise));

            trigger_event_fd();

            return result_future.get();
        }

        EEMResult add_fd(int fd, EEMWorkerDispatchPrep& worker_dispatch) final
        {
            struct epoll_event epoll_control_event
            {
            };

            epoll_control_event.events = EPOLLIN;
            epoll_control_event.data.fd = fd;

            //  Call epoll_ctl to add the fd to the interest set.  If the add
            //      fails because the fd already exists in the set, then simply
            //      log a message and continue on to record the callback.

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &epoll_control_event) < 0)
            {
                SPDLOG_ERROR(
                    "Error adding descriptor to epoll_ctl in "
                    "EpollEventManager::add_fd().  errno = "
                    "{} : {}",
                    errno, strerror(errno));

                if (errno != EEXIST)
                {
                    std::string message(
                        "Unable to add descriptor to epoll_ctl in EpollEventManager::add_fd().  errno = ");
                    message += strerror(errno);

                    return EEMResult::failure(message);
                }
            }

            //  Record the desired callback and return success

            worker_dispatchers_.insert_or_assign(fd, worker_dispatch);

            return EEMResult::success();
        }

        EEMResult modify_fd(int fd, EEMWorkerDispatchPrep& worker_dispatch) final
        {
            struct epoll_event epoll_control_event
            {
            };

            epoll_control_event.events = EPOLLIN;
            epoll_control_event.data.fd = fd;

            //  Call epoll_ctl to modify the fd to the interest set.  If the modify
            //      fails because the fd already exists in the set, then return a failed status.
            //
            //  If the fd has already been added - then this should be a pretty useless call as
            //      none of the parameters are changing but I think it makes sense to do anyway
            //      simpy to be sure of the state of the fd.

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &epoll_control_event) < 0)
            {
                SPDLOG_ERROR(
                    "Unable to modify file descriptor in epoll_ctl in "
                    "EpollEventManager::modify_fd().  errno = "
                    "{} : {}",
                    errno, strerror(errno));

                std::string message(
                    "Unable to modify descriptor to epoll_ctl in EpollEventManager::modify_fd().  errno = ");
                message += strerror(errno);

                return EEMResult::failure(message);
            }

            //  Save the dispatch function

            worker_dispatchers_.insert_or_assign(fd, worker_dispatch);

            return EEMResult::success();
        }

        EEMResult remove_fd(int fd) final
        {
            worker_dispatchers_.erase(fd);

            struct epoll_event epoll_control_event
            {
            };

            epoll_control_event.events = static_cast<uint32_t>(NULL);
            epoll_control_event.data.fd = fd;

            //  Call epoll_ctl to add the pin's fd to the interest set.  If the add
            //  fails because the
            //      fd already exists in the set, then use EPOLL_CTL_MOD to change the
            //      event.  This should never happen - if it does then something is
            //      wrong in this library.

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &epoll_control_event) < 0)
            {
                SPDLOG_ERROR(
                    "Unable to delete file descriptor from epoll_ctl in "
                    "EpollEventManager::remove_fd().  errno = "
                    "{} : {}",
                    errno, strerror(errno));

                std::string message(
                    "Unable to remove descriptor to epoll_ctl in EpollEventManager::remove_fd().  errno = ");
                message += strerror(errno);

                return EEMResult::failure(message);
            }

            return EEMResult::success();
        }

        void enqueue_callback(const std::function<void()>& callback) final
        {
            worker_queue_.enqueue(std::move(WorkerDirective(callback)));
        }
        void enqueue_callback(std::function<void()>&& callback) final
        {
            worker_queue_.enqueue(std::move(WorkerDirective(std::move(callback))));
        }

       private:
        class WorkerDirective
        {
           public:
            enum class Action
            {
                NO_OPERATION = 0,
                SHUTDOWN,
                DISPATCH_EVENT
            };

            WorkerDirective() : action_(Action::NO_OPERATION) {}
            explicit WorkerDirective(Action action_) : action_(action_) {}
            explicit WorkerDirective(const std::function<void()>& event_handler)
                : action_(Action::DISPATCH_EVENT), event_handler_(event_handler)
            {
            }
            explicit WorkerDirective(std::function<void()>&& event_handler) noexcept
                : action_(Action::DISPATCH_EVENT), event_handler_(std::move(event_handler))
            {
            }

            WorkerDirective(WorkerDirective&& directive_to_move) noexcept
                : action_(directive_to_move.action_), event_handler_(std::move(directive_to_move.event_handler_))
            {
            }

            WorkerDirective(const WorkerDirective&) = delete;

            ~WorkerDirective() = default;

            WorkerDirective& operator=(const WorkerDirective& directive_to_move) = delete;

            WorkerDirective& operator=(WorkerDirective&& directive_to_move) noexcept
            {
                action_ = directive_to_move.action_;
                event_handler_ = std::move(directive_to_move.event_handler_);

                return (*this);
            }

            Action action() const { return action_; }

            void dispatch_event() { event_handler_(); }

           private:
            Action action_;
            std::function<void()> event_handler_;
        };

        static constexpr int SERVICE_ROUTINE_THREAD_PRIORITY = 5;
        static constexpr int WORKER_THREAD_PRIORITY = 10;

        static constexpr int TEN_SECONDS_IN_MILLISECONDS = 10000;

        int event_fd_;
        int epoll_fd_;

        bool realtime_scheduling_;

        uint32_t max_number_of_fds_;
        uint32_t epoll_wait_timeout_in_ms_;
        std::atomic<uint64_t> num_events_dispatched_;

        std::thread epoll_service_thread_;
        std::thread worker_thread_;

        std::atomic_bool service_routine_running_;
        std::atomic_bool worker_routine_running_;

        std::map<int, std::reference_wrapper<EEMWorkerDispatchPrep>> worker_dispatchers_;

        moodycamel::ConcurrentQueue<std::pair<EEMDirective<R>*, std::promise<R>*>> isr_directives_;
        moodycamel::BlockingConcurrentQueue<WorkerDirective> worker_queue_;

        void trigger_event_fd()
        {
            uint64_t u = 1;
            auto bytes_written = write(event_fd_, &u, sizeof(uint64_t));
            assert(bytes_written == sizeof(uint64_t));
        }

        void set_realtime_scheduling(int priority)
        {
            if (realtime_scheduling_)
            {
                struct sched_param thread_scheduling
                {
                    .sched_priority = priority
                };

                int set_sched_result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &thread_scheduling);

                if (set_sched_result != 0)
                {
                    SPDLOG_ERROR(
                        "Unable to set ISR Thread scheduling policy - file may need "
                        "cap_sys_nice capability.  Error: "
                        "{}",
                        set_sched_result);
                }
            }
        }

        void epoll_service_routine()
        {
            SPDLOG_TRACE("In epoll_service_routine");

            struct epoll_event epoll_wait_events[max_number_of_fds_ + 2];  //  NOLINT

            constexpr int read_buffer_size = 64;
            char read_buffer[read_buffer_size];  //  NOLINT

            //  Setup realtime scheduling if requested

            set_realtime_scheduling(SERVICE_ROUTINE_THREAD_PRIORITY);

            //  Start the worker thread

            worker_thread_ = std::thread(&EpollEventManager<R, Ex>::worker_main, this);

            //  The main loop for epoll

            service_routine_running_ = true;

            while (service_routine_running_)
            {
                //                    std::cout << "waiting on epoll_wait" << std::endl;

                int event_count =
                    epoll_wait(epoll_fd_, &epoll_wait_events[0], max_number_of_fds_ + 1, epoll_wait_timeout_in_ms_);

                //  Process any events - there may be none if epoll_wait timed out.
                //      We will do this in two passes, first processing any events that
                //      are not from the event_fd and then any events for the event_fd.
                //      This way we avoid any situations where the event_fd triggers
                //      closing of an fd which currently has activity.

                bool event_fd_ready = false;

                for (int i = 0; (service_routine_running_) && (i < event_count); i++)
                {
                    //  If the event is for event_fd_, flag it and we will process it below.
                    //      Otherwise, if it is an EPOLLIN event from another fd then call
                    //      the callback prep routine.

                    if (epoll_wait_events[i].data.fd == event_fd_)
                    {
                        event_fd_ready = true;
                    }
                    else if (epoll_wait_events[i].events == EPOLLIN)
                    {
                        //  Insure there is a valid callback routine for the fd.  If not write a message.

                        auto element = worker_dispatchers_.find(epoll_wait_events[i].data.fd);

                        if (element != worker_dispatchers_.end())
                        {
                            element->second.get().prepare_worker_callback(epoll_wait_events[i].data.fd, *this);
                        }
                        else
                        {
                            int bad_fd = epoll_wait_events[i].data.fd;

                            SPDLOG_ERROR(
                                "In EpollEventManager::epoll_service_routine() - Could not find callback method "
                                "for fd : {}",
                                bad_fd);
                        }
                    }
                }

                if (event_fd_ready)
                {
                    //  We are simply using the event_fd_ to signal that work is available
                    //  in the queue so read from the fd to clear it - but we don't care what is read.

                    auto num_chars_read = read(event_fd_, &read_buffer[0], read_buffer_size);

                    //  Look for directives.  Since this is a blocking call, the caller assumes ownership of the
                    //  directive.  Therefore no need to delete below.

                    std::pair<EEMDirective<R>*, std::promise<R>*> new_directive;

                    while (isr_directives_.try_dequeue(new_directive))
                    {
                        R result = new_directive.first->handle_directive(*this);
                        new_directive.second->set_value(result);
                    }
                }
            }

            //  Ask the worker thread to shut itself down and wait for it to join

            worker_queue_.enqueue(WorkerDirective(WorkerDirective::Action::SHUTDOWN));

            worker_thread_.join();

            SPDLOG_TRACE("Exiting epoll_service_routine");
        };

        void worker_main()
        {
            SPDLOG_DEBUG("EpollEventManager worker thread starting");

            //  Set realtime scheduling if requested

            set_realtime_scheduling(WORKER_THREAD_PRIORITY);

            //  Start the worker dispatch loop

            worker_routine_running_ = true;

            //  Loop until we are asked to shutdown this thread

            while (worker_routine_running_)
            {
                //  We use a blocking queue, so we have to wait for an event to arrive
                //  before doing anything.  There is a timeout, so in the event that the queue gets messed up,
                //  the thread should still exit if the worker_routine_running_ member variable is false.

                WorkerDirective directive;

                if (worker_queue_.wait_dequeue_timed(directive, std::chrono::seconds(1)) && worker_routine_running_)
                {
                    //  There are only two directives - shutdown or dispatch an event to a
                    //  pin ISR

                    if (directive.action() == WorkerDirective::Action::SHUTDOWN)
                    {
                        SPDLOG_DEBUG("EpollEventManager worker thread shutdown requested");

                        worker_routine_running_ = false;
                    }
                    else if (directive.action() == WorkerDirective::Action::DISPATCH_EVENT)
                    {
                        num_events_dispatched_++;

                        //  The callback is simply a void() method - so no need to worry about
                        //  arguments.  Typically, std::bind will be used to package up all the
                        //  arguments before handing off the function as void().

                        directive.dispatch_event();
                    }
                }
            }

            SPDLOG_DEBUG("EpollEventManager worker thread exiting");
        }
    };
}  // namespace SEFUtility::EEM
