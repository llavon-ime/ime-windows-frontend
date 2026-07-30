#include "ui/xamlCompatibility.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

#include "utils/debugSink.hpp"

namespace tsf {
namespace {

using QuirkIsEnabledFn = BOOL(WINAPI*)(DWORD);

constexpr DWORD xaml_islands_manifest_quirk = 0x20106;
constexpr char quirks_api_set[] = "api-ms-win-core-quirks-l1-1-0.dll";
constexpr char quirk_function[] = "QuirkIsEnabled";

std::atomic<QuirkIsEnabledFn> original_quirk_is_enabled{nullptr};
std::atomic<LONG> g_intercepted_target_calls{0};

// Compatibility research record (July 2026):
//
// Windows.UI.Xaml.dll gates DesktopWindowXamlSource initialization through
// QuirkIsEnabled(0x20106). The result comes from process compatibility state
// populated from the executable's startup manifest. A runtime activation
// context, a new STA thread, or a separate child HWND does not update that
// state. On the investigated build, the quirk is enabled when the host omits
// maxversiontested (or declares a value older than 10.0.18226.0), causing
// WindowsXamlManager::InitializeForCurrentThread to fail with E_UNEXPECTED.
//
// An x64 end-to-end test on OS 10.0.26200.0 with Windows.UI.Xaml.dll
// 10.0.26100.8875 established that overriding only quirk 0x20106 allows the
// manager, DesktopWindowXamlSource, AttachToWindow, content, message pump, and
// clean shutdown to complete. All other quirk IDs are forwarded unchanged.
//
// Risk record:
// This is an unsupported, build-dependent compatibility workaround using a
// private Windows implementation detail. It temporarily changes one pointer in
// Windows.UI.Xaml.dll's own IAT inside the host process, so integrity/security
// software can observe it and another IAT hook could conflict with it. To
// minimize that risk, installation is skipped unless the original quirk result
// says the workaround is needed; the expected import API set, function, and
// original KERNELBASE target are all validated; calls other than 0x20106 are
// forwarded; and the original IAT pointer/protection are restored immediately
// after the initial XAML objects are created. Any structural mismatch fails
// closed and leaves the IAT untouched.
BOOL WINAPI xaml_quirk_is_enabled_hook(DWORD quirk_id) noexcept {
    const auto original = original_quirk_is_enabled.load(std::memory_order_acquire);
    if (quirk_id == xaml_islands_manifest_quirk) {
        g_intercepted_target_calls.fetch_add(1, std::memory_order_relaxed);
        return FALSE;
    }
    return original != nullptr ? original(quirk_id) : FALSE;
}

bool range_is_inside_image(std::size_t offset, std::size_t length, std::size_t image_size) noexcept {
    return offset <= image_size && length <= image_size - offset;
}

bool bounded_ascii_equals(
    const std::byte* image,
    std::size_t image_size,
    std::size_t offset,
    const char* expected) noexcept {
    const std::size_t expected_length = std::strlen(expected);
    if (!range_is_inside_image(offset, expected_length + 1, image_size)) {
        return false;
    }

    const auto value = reinterpret_cast<const char*>(image + offset);
    return _strnicmp(value, expected, expected_length) == 0 && value[expected_length] == '\0';
}

bool is_expected_quirk_target(const void* address) noexcept {
    const HMODULE kernelbase = GetModuleHandleW(L"KERNELBASE.dll");
    if (kernelbase == nullptr) {
        return false;
    }
    return reinterpret_cast<const void*>(GetProcAddress(kernelbase, quirk_function)) == address;
}

void* volatile* find_quirk_iat_slot(HMODULE xaml_module) noexcept {
    if (xaml_module == nullptr) {
        return nullptr;
    }

    const auto image = reinterpret_cast<const std::byte*>(xaml_module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }

    constexpr std::size_t maximum_reasonable_nt_header_offset = 1024 * 1024;
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    if (nt_offset > maximum_reasonable_nt_header_offset) {
        return nullptr;
    }

    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic !=
#ifdef _WIN64
            IMAGE_NT_OPTIONAL_HDR64_MAGIC
#else
            IMAGE_NT_OPTIONAL_HDR32_MAGIC
#endif
    ) {
        return nullptr;
    }

    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    if (!range_is_inside_image(nt_offset, sizeof(*nt), image_size)) {
        return nullptr;
    }

    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0 || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !range_is_inside_image(imports.VirtualAddress, imports.Size, image_size)) {
        return nullptr;
    }

    const std::size_t descriptor_count = imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const auto descriptors =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + imports.VirtualAddress);
    for (std::size_t descriptor_index = 0; descriptor_index < descriptor_count; ++descriptor_index) {
        const auto& descriptor = descriptors[descriptor_index];
        if (descriptor.Name == 0) {
            break;
        }
        if (!bounded_ascii_equals(image, image_size, descriptor.Name, quirks_api_set)) {
            continue;
        }
        if (descriptor.OriginalFirstThunk == 0 || descriptor.FirstThunk == 0) {
            return nullptr;
        }

        const std::size_t name_thunk_start = descriptor.OriginalFirstThunk;
        const std::size_t iat_thunk_start = descriptor.FirstThunk;
        if (name_thunk_start >= image_size || iat_thunk_start >= image_size) {
            return nullptr;
        }
        const std::size_t maximum_thunks = (std::min)(
            (image_size - name_thunk_start) / sizeof(IMAGE_THUNK_DATA),
            (image_size - iat_thunk_start) / sizeof(IMAGE_THUNK_DATA));

        for (std::size_t thunk_index = 0; thunk_index < maximum_thunks; ++thunk_index) {
            const std::size_t thunk_offset = thunk_index * sizeof(IMAGE_THUNK_DATA);
            const std::size_t name_thunk_rva = name_thunk_start + thunk_offset;
            const std::size_t iat_thunk_rva = iat_thunk_start + thunk_offset;
            if (!range_is_inside_image(name_thunk_rva, sizeof(IMAGE_THUNK_DATA), image_size) ||
                !range_is_inside_image(iat_thunk_rva, sizeof(IMAGE_THUNK_DATA), image_size)) {
                return nullptr;
            }

            const auto name_thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(image + name_thunk_rva);
            if (name_thunk->u1.AddressOfData == 0) {
                return nullptr;
            }
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }

            const std::size_t import_rva = static_cast<std::size_t>(name_thunk->u1.AddressOfData);
            constexpr std::size_t import_name_offset = offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (!range_is_inside_image(import_rva, import_name_offset + sizeof(char), image_size) ||
                !bounded_ascii_equals(image, image_size, import_rva + import_name_offset, quirk_function)) {
                continue;
            }

            auto iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                const_cast<std::byte*>(image) + iat_thunk_rva);
            static_assert(sizeof(iat_thunk->u1.Function) == sizeof(void*));
            return reinterpret_cast<void* volatile*>(&iat_thunk->u1.Function);
        }
        return nullptr;
    }

    return nullptr;
}

void log_workaround(const std::wstring& message) {
    DebugSink::instance().send(L"UI", L"XamlMaxVersionTestedWorkaround " + message);
}

}  // namespace

std::mutex& ScopedXamlMaxVersionTestedWorkaround::process_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

ScopedXamlMaxVersionTestedWorkaround::ScopedXamlMaxVersionTestedWorkaround()
    : process_lock_(process_mutex()) {
    // Keep one process-lifetime reference. XAML objects outlive this guard, and
    // retaining the system module avoids unloading it immediately after its IAT
    // has been restored.
    static const HMODULE xaml_module =
        LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xaml_module == nullptr) {
        log_workaround(L"skipped: LoadLibraryExW failed err=" + std::to_wstring(GetLastError()));
        return;
    }

    iat_slot_ = find_quirk_iat_slot(xaml_module);
    if (iat_slot_ == nullptr) {
        log_workaround(L"skipped: expected XAML import was not found");
        return;
    }

    original_target_ = *iat_slot_;
    if (!is_expected_quirk_target(original_target_)) {
        log_workaround(L"skipped: IAT target is not the expected KERNELBASE export");
        iat_slot_ = nullptr;
        original_target_ = nullptr;
        return;
    }

    const auto original = reinterpret_cast<QuirkIsEnabledFn>(original_target_);
    original_quirk_is_enabled.store(original, std::memory_order_release);
    required_ = original(xaml_islands_manifest_quirk) != FALSE;
    if (!required_) {
        log_workaround(L"not required by host compatibility state");
        iat_slot_ = nullptr;
        original_target_ = nullptr;
        return;
    }

    g_intercepted_target_calls.store(0, std::memory_order_relaxed);

    HMODULE self_reference = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&xaml_quirk_is_enabled_hook),
            &self_reference) == FALSE) {
        log_workaround(L"install failed: could not retain hook module");
        iat_slot_ = nullptr;
        original_target_ = nullptr;
        return;
    }

    DWORD old_protect = 0;
    if (VirtualProtect(
            const_cast<void**>(iat_slot_),
            sizeof(*iat_slot_),
            PAGE_READWRITE,
            &old_protect) == FALSE) {
        log_workaround(L"install failed: VirtualProtect err=" + std::to_wstring(GetLastError()));
        FreeLibrary(self_reference);
        iat_slot_ = nullptr;
        original_target_ = nullptr;
        return;
    }

    void* const hook_target = reinterpret_cast<void*>(&xaml_quirk_is_enabled_hook);
    void* const observed = InterlockedCompareExchangePointer(
        iat_slot_,
        hook_target,
        original_target_);
    if (observed != original_target_) {
        DWORD ignored = 0;
        VirtualProtect(const_cast<void**>(iat_slot_), sizeof(*iat_slot_), old_protect, &ignored);
        log_workaround(L"install skipped: IAT changed concurrently");
        FreeLibrary(self_reference);
        iat_slot_ = nullptr;
        original_target_ = nullptr;
        return;
    }

    FlushInstructionCache(GetCurrentProcess(), const_cast<void**>(iat_slot_), sizeof(*iat_slot_));

    DWORD ignored = 0;
    if (VirtualProtect(
            const_cast<void**>(iat_slot_),
            sizeof(*iat_slot_),
            old_protect,
            &ignored) == FALSE) {
        const DWORD protect_error = GetLastError();
        InterlockedCompareExchangePointer(iat_slot_, original_target_, hook_target);
        FlushInstructionCache(GetCurrentProcess(), const_cast<void**>(iat_slot_), sizeof(*iat_slot_));
        VirtualProtect(const_cast<void**>(iat_slot_), sizeof(*iat_slot_), old_protect, &ignored);
        log_workaround(
            L"install rolled back: could not restore page protection err=" + std::to_wstring(protect_error));
        FreeLibrary(self_reference);
        iat_slot_ = nullptr;
        original_target_ = nullptr;
        return;
    }

    self_module_reference_ = self_reference;
    installed_ = true;
    log_workaround(L"installed for quirk 0x20106");
}

ScopedXamlMaxVersionTestedWorkaround::~ScopedXamlMaxVersionTestedWorkaround() noexcept {
    restore();
}

LONG ScopedXamlMaxVersionTestedWorkaround::intercepted_target_calls() const noexcept {
    return installed_ ? g_intercepted_target_calls.load(std::memory_order_relaxed) : 0;
}

void ScopedXamlMaxVersionTestedWorkaround::restore() noexcept {
    if (!installed_ || iat_slot_ == nullptr || original_target_ == nullptr) {
        return;
    }

    DWORD old_protect = 0;
    if (VirtualProtect(
            const_cast<void**>(iat_slot_),
            sizeof(*iat_slot_),
            PAGE_READWRITE,
            &old_protect) == FALSE) {
        log_workaround(L"restore failed: VirtualProtect err=" + std::to_wstring(GetLastError()));
        return;
    }

    void* const hook_target = reinterpret_cast<void*>(&xaml_quirk_is_enabled_hook);
    void* const observed =
        InterlockedCompareExchangePointer(iat_slot_, original_target_, hook_target);
    FlushInstructionCache(GetCurrentProcess(), const_cast<void**>(iat_slot_), sizeof(*iat_slot_));

    DWORD ignored = 0;
    const BOOL protection_restored =
        VirtualProtect(const_cast<void**>(iat_slot_), sizeof(*iat_slot_), old_protect, &ignored);

    installed_ = false;
    if (self_module_reference_ != nullptr) {
        FreeLibrary(self_module_reference_);
        self_module_reference_ = nullptr;
    }
    if (observed == hook_target && protection_restored != FALSE) {
        log_workaround(
            L"restored after target_calls=" +
            std::to_wstring(g_intercepted_target_calls.load(std::memory_order_relaxed)));
        return;
    }

    if (observed != hook_target) {
        log_workaround(L"restore did not overwrite a concurrently changed IAT");
    }
    if (protection_restored == FALSE) {
        log_workaround(L"restore warning: page protection err=" + std::to_wstring(GetLastError()));
    }
}

}  // namespace tsf
