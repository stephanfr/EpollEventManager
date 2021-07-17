#pragma once

#include "Result.hpp"



enum class EMMTestResultCodes
{
    UNINITIALIZED = -1,
    SUCCESS = 0,
    FAILURE
};

class EEMTestResult
{
   public:
    EEMTestResult() : result_(SEFUtility::Result<EMMTestResultCodes>::failure(EMMTestResultCodes::UNINITIALIZED, "")) {}

    EEMTestResult(const EEMTestResult& result_to_copy) = default;

    EEMTestResult(EEMTestResult&& result_to_copy) noexcept : result_(std::move(result_to_copy.result_)) {}      //  NOLINT

    ~EEMTestResult() = default;

    EEMTestResult& operator=(const EEMTestResult& result_to_copy) = default;

    EEMTestResult& operator=(EEMTestResult&& result_to_move) noexcept = delete;


    bool succeeded() { return result_.succeeded(); }
    bool failed() { return result_.failed(); }

    static EEMTestResult success() { return EEMTestResult(SEFUtility::Result<EMMTestResultCodes>::success()); }

    static EEMTestResult failure(EMMTestResultCodes error_code, const std::string& message)
    {
        return EEMTestResult(SEFUtility::Result<EMMTestResultCodes>::failure(error_code, message));
    }

    [[nodiscard]] const std::string&      message() const { return result_.message(); }

   private:
    SEFUtility::Result<EMMTestResultCodes> result_;

    explicit EEMTestResult(const SEFUtility::Result<EMMTestResultCodes>& result) : result_(result) {}
};
