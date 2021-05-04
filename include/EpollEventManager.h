#pragma once

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>

#include <blockingconcurrentqueue.h>
#include <concurrentqueue.h>
#include <readerwriterqueue.h>


#include "spdlog/spdlog.h"

namespace SEFUtility
{
    //
    //  R is the result class for any dispatch results
    //  Ex is the Exception class which defaults to std::runtime_error
    //

    template <typename R, typename Ex = std::runtime_error>
    class EpollEventManager
    {
       public:
        //  The Directive class is the base for any instructions sent from external code to
        //      the subclass of the EpollEventManager.

        class Directive
        {
           public:
            Directive() = default;

            virtual ~Directive() = default;

            virtual R handle_directive(EpollEventManager<R, Ex>& epoll_event_manager) = 0;

            void send_response(R& result) { response_queue_.emplace(result); }

           private:
            moodycamel::BlockingReaderWriterQueue<R> response_queue_;

            R wait_response()
            {
                R response;

                response_queue_.wait_dequeue(response);

                return response;
            }

            friend class EpollEventManager<R, Ex>;
        };

        //  The WorkerDispatchPrep class is called from the epoll service routine and returns
        //      a std::function<void()> that will be called from the worker routine.  This is provided
        //      to give descendent classes the opportunity to read or write to the fd either prior
        //      to the function hand off to the worker thread or within the worker thread itself.
        //      Reading in the worker thread can lead to timing issues for some use cases.

        class WorkerDispatchPrep
        {
           public:
            WorkerDispatchPrep(EpollEventManager<R, Ex>& event_manager) : event_manager_(event_manager){};
            virtual ~WorkerDispatchPrep() = default;

            virtual void prepare_worker_callback() = 0;

           protected:
            void enqueue_callback(std::function<void()>& callback) { event_manager_.enqueue_worker_callback(callback); }
            void enqueue_callback(std::function<void()>&& callback)
            {
                event_manager_.enqueue_worker_callback(std::move(callback));
            }

           private:
            EpollEventManager<R, Ex>& event_manager_;
        };

        //  EpollEventManager provides a framework for responsing to activity on file descriptors using
        //      epoll with the ability to offload callbacks and pass control instructions to the service routine.

        EpollEventManager(uint32_t max_number_of_fds, bool realtime_scheduling = false,
                          bool autostart_service_routine = true)
            : max_number_of_fds_(max_number_of_fds),
              realtime_scheduling_(realtime_scheduling),
              service_routine_running_(false),
              worker_routine_running_(false),
              num_events_dispatched_(0),
              epoll_wait_timeout_in_ms_(10000),
              event_fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
              epoll_fd_(epoll_create1(EPOLL_CLOEXEC))
        {
            //  Add the event file descriptor so that we can gracefully interrupt the epoll_wait.  Fail immediately,
            //      if for some reason it cannot be added.

            struct epoll_event epoll_control_event;

            epoll_control_event.events = EPOLLIN;
            epoll_control_event.data.fd = event_fd_;

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &epoll_control_event) < 0)
            {
                SPDLOG_CRITICAL(
                    "Unable to add event fd to epoll_ctl in ISRHandler::interrupt_service_routine().  errno = {}",
                    errno);
                throw Ex("Unable to add event fd to epoll_ctl in ISRHandler::interrupt_service_routine()");
            }

            //  Autostart the service routine if requested

            if (autostart_service_routine)
            {
                start_service_routine();
            }
        }

        virtual ~EpollEventManager()
        {
            //  Shutdown the service routine if it is still running

            shutdown_service_routine();
        }

        uint64_t num_events_dispatched() const { return num_events_dispatched_; }

        void start_service_routine()
        {
            //  Return immediately doing nothing if the epoll service thread is already running

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

            //  Set the service routine runnign falg false and trigger the event file descriptor

            service_routine_running_ = false;

            trigger_event_fd();

            //  Wait for the service thread to join

            epoll_service_thread_.join();
        }

        template <typename D>
        R send_directive(D& directive)
        {
            static_assert(std::is_convertible<D*, Directive*>::value,
                          "EpollEventManager::send_directive template argument 'D' must be covertible to "
                          "EpollEventManager::Directive");

            isr_directives_.enqueue(&directive);

            trigger_event_fd();

            return dynamic_cast<Directive&>(directive).wait_response();
        }

        std::pair<bool, int> add_fd(int fd, WorkerDispatchPrep& worker_dispatch)
        {
            worker_dispatchers_.emplace(fd, worker_dispatch);

            struct epoll_event epoll_control_event;
            epoll_control_event.events = EPOLLIN;
            epoll_control_event.data.fd = fd;

            //  Call epoll_ctl to add the pin's fd to the interest set.  If the add fails because the
            //      fd already exists in the set, then use EPOLL_CTL_MOD to change the event.  This
            //      should never happen - if it does then something is wrong in this library.

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &epoll_control_event) < 0)
            {
                SPDLOG_ERROR(
                    "Unable to add descriptor to epoll_ctl in "
                    "EpollEventManager::add_fd().  errno = "
                    "{}",
                    errno);

                worker_dispatchers_.erase(fd);

                return std::make_pair(false, errno);
            }

            return std::make_pair(true, 0);
        }

        std::pair<bool, int> modify_fd(int fd, WorkerDispatchPrep& worker_dispatch)
        {
            worker_dispatchers_.emplace(fd, worker_dispatch);

            struct epoll_event epoll_control_event;
            epoll_control_event.events = EPOLLIN;
            epoll_control_event.data.fd = fd;

            //  Call epoll_ctl to add the pin's fd to the interest set.  If the add fails because the
            //      fd already exists in the set, then use EPOLL_CTL_MOD to change the event.  This
            //      should never happen - if it does then something is wrong in this library.

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &epoll_control_event) < 0)
            {
                SPDLOG_ERROR(
                    "Unable to modify file descriptor in epoll_ctl in "
                    "EpollEventManager::modify_fd().  errno = "
                    "{}",
                    errno);

                return std::make_pair(false, errno);
            }

            return std::make_pair(true, 0);
        }

        std::pair<bool, int> remove_fd(int fd)
        {
            worker_dispatchers_.erase(fd);

            struct epoll_event epoll_control_event;
            epoll_control_event.events = static_cast<uint32_t>(NULL);
            epoll_control_event.data.fd = fd;

            //  Call epoll_ctl to add the pin's fd to the interest set.  If the add fails because the
            //      fd already exists in the set, then use EPOLL_CTL_MOD to change the event.  This
            //      should never happen - if it does then something is wrong in this library.

            if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &epoll_control_event) < 0)
            {
                SPDLOG_ERROR(
                    "Unable to delete file descriptor from epoll_ctl in "
                    "EpollEventManager::remove_fd().  errno = "
                    "{}",
                    errno);

                return std::make_pair(false, errno);
            }

            return std::make_pair(true, 0);
        }

       private:
        friend class WorkerDispatchPrep;

        class FileDescriptorGuard
        {
           public:
            explicit FileDescriptorGuard(int fd) : fd_(fd)
            {
                if (fd < 0)
                {
                    SPDLOG_CRITICAL("File Descriptor is less than zero.  errno: {}", errno);
                    throw Ex("FileDescriptorGuard: File Descriptor is less than zero.");
                }
            }

            FileDescriptorGuard(const FileDescriptorGuard&) = delete;
            FileDescriptorGuard(FileDescriptorGuard&&) = delete;

            FileDescriptorGuard& operator=(const FileDescriptorGuard&) = delete;
            FileDescriptorGuard& operator=(FileDescriptorGuard&&) = delete;

            ~FileDescriptorGuard() { close(); }

            operator int() { return (fd_); }

            int fd() const { return (fd_); }

            void close()
            {
                if (fd_ >= 0)
                {
                    ::close(fd_);
                }

                fd_ = -1;
            }

           private:
            int fd_;
        };

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
            WorkerDirective(Action action_) : action_(action_) {}
            WorkerDirective(std::function<void()>& event_handler)
                : action_(Action::DISPATCH_EVENT), event_handler_(event_handler)
            {
            }
            WorkerDirective(std::function<void()>&& event_handler)
                : action_(Action::DISPATCH_EVENT), event_handler_(std::move(event_handler))
            {
            }

            WorkerDirective(WorkerDirective&& directive_to_move)
                : action_(directive_to_move.action_), event_handler_(std::move(directive_to_move.event_handler_))
            {
            }

            WorkerDirective& operator=(WorkerDirective&& directive_to_move)
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

        FileDescriptorGuard event_fd_;
        FileDescriptorGuard epoll_fd_;

        bool realtime_scheduling_;

        uint32_t max_number_of_fds_;
        uint32_t epoll_wait_timeout_in_ms_;
        std::atomic<uint64_t> num_events_dispatched_;

        std::thread epoll_service_thread_;
        std::thread worker_thread_;

        std::atomic_bool service_routine_running_;
        std::atomic_bool worker_routine_running_;

        std::map<int, std::reference_wrapper<WorkerDispatchPrep>> worker_dispatchers_;

        moodycamel::ConcurrentQueue<Directive*> isr_directives_;
        moodycamel::BlockingConcurrentQueue<WorkerDirective> worker_queue_;

        void trigger_event_fd()
        {
            uint64_t u = 1;
            write(event_fd_, &u, sizeof(uint64_t));
        }

        void enqueue_worker_callback(std::function<void()>& callback) { worker_queue_.enqueue(callback); }
        void enqueue_worker_callback(std::function<void()>&& callback) { worker_queue_.enqueue(callback); }

        void epoll_service_routine()
        {
            SPDLOG_TRACE("In epoll_service_routine");

            //  Use realtime scheduling if requested

            if (realtime_scheduling_)
            {
                struct sched_param thread_scheduling;

                thread_scheduling.sched_priority = 5;

                int set_sched_result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &thread_scheduling);

                if (set_sched_result != 0)
                {
                    SPDLOG_ERROR(
                        "Unable to set ISR Thread scheduling policy - file may need cap_sys_nice capability.  Error: "
                        "{}",
                        set_sched_result);
                }
            }

            struct epoll_event epoll_control_event;
            struct epoll_event epoll_wait_events[max_number_of_fds_ + 2];

            int event_count;

            constexpr int read_buffer_size = 64;
            char read_buffer[read_buffer_size];

            //  Start the worker thread and use realtime scheduling if it was requested

            worker_thread_ = std::thread(&EpollEventManager<R, Ex>::worker_main, this);

            //  The main loop for epoll

            service_routine_running_ = true;

            while (service_routine_running_)
            {
                event_count =
                    epoll_wait(epoll_fd_, &epoll_wait_events[0], max_number_of_fds_ + 1, epoll_wait_timeout_in_ms_);

                //  Process any events - there may be none if epoll_wait timed out.
                //      We will do this in two passes, first processing any events that are not
                //      from the event_fd and then any events for the event_fd.  This way we avoid
                //      any situations where the event_fd triggers closing of an fd which currently
                //      has activity.

                bool event_fd_ready = false;

                for (int i = 0; (service_routine_running_) && (i < event_count); i++)
                {
                    //  If the event is for event_fd_, read from the fd and look for directives in the queue
                    //      Otherwise, if it is an EPOLLIN event from another fd then call the callback prep routine.

                    if ((epoll_wait_events[i].data.fd == event_fd_) && (epoll_wait_events[i].events == EPOLLIN))
                    {
                        event_fd_ready = true;
                    }
                    else if (epoll_wait_events[i].events == EPOLLIN)
                    {
                        worker_dispatchers_.at(epoll_wait_events[i].data.fd).get().prepare_worker_callback();
                    }
                }

                if (event_fd_ready)
                {
                    //  We are simply using the event_fd_ to signal that work is available in the queue
                    //      so read from the fd to clear it - but we don't care what is read.

                    read(event_fd_, &read_buffer[0], read_buffer_size);

                    //  Look for directives
                    //  Since this is a blocking call, the caller assumes ownership of the directive.  Therefore
                    //      no need to delete below.

                    Directive* new_directive;

                    while (isr_directives_.try_dequeue(new_directive))
                    {
                        R result = new_directive->handle_directive(*this);
                        new_directive->send_response(result);
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

            if (realtime_scheduling_)
            {
                struct sched_param thread_scheduling;

                thread_scheduling.sched_priority = 10;

                int set_sched_result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &thread_scheduling);

                if (set_sched_result != 0)
                {
                    SPDLOG_ERROR(
                        "Unable to set ISR Worker Thread scheduling policy - file may need cap_sys_nice capability.  "
                        "Error: {}",
                        set_sched_result);
                }
            }

            //  Start the worker dispatch loop

            worker_routine_running_ = true;

            //  Loop until we are asked to shutdown this thread

            while (worker_routine_running_)
            {
                //  We use a blocking queue, so we have to wait for an event to arrive before doing anything.
                //      There is a timeout, so in the event that the queue gets messed up, the thread should still
                //      exit if the worker_routine_running_ member variable is false.

                WorkerDirective directive;

                if (worker_queue_.wait_dequeue_timed(directive, std::chrono::seconds(1)) && worker_routine_running_)
                {
                    //  There are only two directives - shutdown or dispatch an event to a pin ISR

                    if (directive.action() == WorkerDirective::Action::SHUTDOWN)
                    {
                        SPDLOG_DEBUG("EpollEventManager worker thread shutdown requested");

                        worker_routine_running_ = false;
                    }
                    else if (directive.action() == WorkerDirective::Action::DISPATCH_EVENT)
                    {
                        num_events_dispatched_++;

                        //  The callback is simply a void() method - so no need to worry about arguments.
                        //      Typically, std::bind will be use to package up all the arguments before
                        //      handing off the function as void().

                        directive.dispatch_event();
                    }
                }
            }

            SPDLOG_DEBUG("EpollEventManager worker thread exiting");
        }
    };  // namespace SEFUtility
}  // namespace SEFUtility