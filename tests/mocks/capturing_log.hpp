// pal::ILog that records every line for assertions.
#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pal/pal_log.hpp"

namespace mth::test
{

class CapturingLog final : public pal::ILog
{
  public:
    void write(pal::LogLevel level, std::string_view message) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.emplace_back(level, std::string(message));
    }

    std::vector<std::pair<pal::LogLevel, std::string>> entries() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

    [[nodiscard]] bool saw(pal::LogLevel level, std::string_view needle) const
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto &[lvl, text] : entries_)
            if (lvl == level && text.find(needle) != std::string::npos)
                return true;
        return false;
    }

  private:
    mutable std::mutex mu_;
    std::vector<std::pair<pal::LogLevel, std::string>> entries_;
};

} // namespace mth::test
