#pragma once

#include <windows.h>

#include <mutex>

namespace tsf {

// Temporarily applies the process-startup compatibility result that XAML
// Islands require, without changing the host executable or its manifest.
class ScopedXamlMaxVersionTestedWorkaround final {
public:
    ScopedXamlMaxVersionTestedWorkaround();
    ~ScopedXamlMaxVersionTestedWorkaround() noexcept;

    ScopedXamlMaxVersionTestedWorkaround(const ScopedXamlMaxVersionTestedWorkaround&) = delete;
    ScopedXamlMaxVersionTestedWorkaround& operator=(const ScopedXamlMaxVersionTestedWorkaround&) = delete;

    [[nodiscard]] bool required() const noexcept {
        return required_;
    }

    [[nodiscard]] bool installed() const noexcept {
        return installed_;
    }

    [[nodiscard]] LONG intercepted_target_calls() const noexcept;

private:
    static std::mutex& process_mutex() noexcept;
    void restore() noexcept;

    std::unique_lock<std::mutex> process_lock_;
    void* volatile* iat_slot_ = nullptr;
    void* original_target_ = nullptr;
    HMODULE self_module_reference_ = nullptr;
    bool required_ = false;
    bool installed_ = false;
};

}  // namespace tsf
