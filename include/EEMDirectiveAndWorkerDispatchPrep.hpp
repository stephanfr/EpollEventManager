#pragma once

#include <functional>

namespace SEFUtility
{
    namespace EEM
    {
        class EEMResult
        {
           public:
            enum class ResultCode
            {
                SUCCESS = 0,
                FAILURE
            };

            EEMResult(ResultCode result_code, const std::string& message)
                : result_code_(result_code), message_(message){};

            bool success() const { return result_code_ == ResultCode::SUCCESS; }

            ResultCode result_code() const { return result_code_; }
            const std::string message() const { return message_; }

            static EEMResult succeeded() { return EEMResult(ResultCode::SUCCESS, ""); }
            static EEMResult failed(const std::string message) { return EEMResult(ResultCode::FAILURE, message); }

           private:
            ResultCode result_code_;
            std::string message_;
        };

        class EEMCallbackQueue
        {
           public:
            virtual void enqueue_callback(std::function<void()>& callback) = 0;
            virtual void enqueue_callback(std::function<void()>&& callback) = 0;
        };

        class EEMWorkerDispatchPrep
        {
           public:
            virtual void prepare_worker_callback(int fd, EEMCallbackQueue& queue) = 0;
        };

        class EEMFileDescriptorManager
        {
            public :
            virtual EEMResult add_fd(int fd, EEMWorkerDispatchPrep& worker_dispatch) = 0;
            virtual EEMResult modify_fd(int fd, EEMWorkerDispatchPrep& worker_dispatch) = 0;
            virtual EEMResult remove_fd(int fd) = 0;
        };

        template <typename R>
        class EEMDirective
        {
           public:
            virtual R handle_directive(EEMFileDescriptorManager& fd_manager) = 0;
        };

    }  // namespace EEM

}  // namespace SEFUtility