#include "inputModeLangBarItem.hpp"

#include <strsafe.h>

namespace tsf {

namespace {

constexpr DWORD kSinkCookie = 1;
constexpr int kIconSize = 16;

}  // namespace

InputModeLangBarItem::InputModeLangBarItem(std::function<void()> on_click) : on_click_(std::move(on_click)) {
    info_.clsidService = Globals::text_service_clsid;
    info_.guidItem = GUID_LBI_INPUTMODE;
    info_.dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_HIDDENSTATUSCONTROL | TF_LBI_STYLE_SHOWNINTRAY;
    info_.ulSort = 0;
    StringCchCopyW(info_.szDescription, ARRAYSIZE(info_.szDescription), L"Input mode");
}

HRESULT InputModeLangBarItem::add_to_language_bar(ITfThreadMgr* thread_mgr) {
    if (!thread_mgr) {
        return E_INVALIDARG;
    }
    if (added_) {
        return S_OK;
    }

    winrt::com_ptr<ITfLangBarItemMgr> manager;
    HRESULT hr = thread_mgr->QueryInterface(IID_PPV_ARGS(manager.put()));
    if (FAILED(hr)) {
        return hr;
    }

    ITfLangBarItem* raw_item = nullptr;
    hr = QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void**>(&raw_item));
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfLangBarItem> item;
    item.attach(raw_item);
    hr = manager->AddItem(item.get());
    if (SUCCEEDED(hr)) {
        added_ = true;
    }
    return hr;
}

HRESULT InputModeLangBarItem::remove_from_language_bar(ITfThreadMgr* thread_mgr) {
    if (!thread_mgr) {
        return E_INVALIDARG;
    }
    if (!added_) {
        return S_OK;
    }

    winrt::com_ptr<ITfLangBarItemMgr> manager;
    HRESULT hr = thread_mgr->QueryInterface(IID_PPV_ARGS(manager.put()));
    if (FAILED(hr)) {
        return hr;
    }

    ITfLangBarItem* raw_item = nullptr;
    hr = QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void**>(&raw_item));
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfLangBarItem> item;
    item.attach(raw_item);
    hr = manager->RemoveItem(item.get());
    if (SUCCEEDED(hr)) {
        added_ = false;
    }
    return hr;
}

void InputModeLangBarItem::set_mode(InputMode mode) {
    if (mode_ == mode) {
        return;
    }

    mode_ = mode;
    notify_update(TF_LBI_TEXT | TF_LBI_ICON | TF_LBI_TOOLTIP);
}

STDMETHODIMP InputModeLangBarItem::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) {
        return E_INVALIDARG;
    }

    *info = info_;
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::GetStatus(DWORD* status) {
    if (!status) {
        return E_INVALIDARG;
    }

    *status = status_;
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::Show(BOOL show) {
    const DWORD old_status = status_;
    if (show) {
        status_ &= ~TF_LBI_STATUS_HIDDEN;
    } else {
        status_ |= TF_LBI_STATUS_HIDDEN;
    }

    if (old_status != status_) {
        notify_update(TF_LBI_STATUS);
    }
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::GetTooltipString(BSTR* tooltip) {
    if (!tooltip) {
        return E_INVALIDARG;
    }

    *tooltip = SysAllocString(tooltip_text());
    return *tooltip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP InputModeLangBarItem::OnClick(TfLBIClick /*click*/, POINT /*point*/, const RECT* /*area*/) {
    if (on_click_) {
        on_click_();
    }
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::InitMenu(ITfMenu* /*menu*/) {
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::OnMenuSelect(UINT /*id*/) {
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::GetIcon(HICON* icon) {
    if (!icon) {
        return E_INVALIDARG;
    }

    *icon = create_mode_icon();
    return *icon ? S_OK : E_FAIL;
}

STDMETHODIMP InputModeLangBarItem::GetText(BSTR* text) {
    if (!text) {
        return E_INVALIDARG;
    }

    *text = SysAllocString(mode_label());
    return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP InputModeLangBarItem::AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) {
    if (!cookie) {
        return E_INVALIDARG;
    }
    *cookie = 0;

    if (!IsEqualIID(riid, IID_ITfLangBarItemSink)) {
        return CONNECT_E_CANNOTCONNECT;
    }
    if (!punk) {
        return E_INVALIDARG;
    }
    if (sink_) {
        return CONNECT_E_ADVISELIMIT;
    }

    ITfLangBarItemSink* raw_sink = nullptr;
    HRESULT hr = punk->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void**>(&raw_sink));
    if (FAILED(hr)) {
        return hr;
    }

    sink_.attach(raw_sink);
    *cookie = kSinkCookie;
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::UnadviseSink(DWORD cookie) {
    if (cookie != kSinkCookie || !sink_) {
        return CONNECT_E_NOCONNECTION;
    }

    sink_ = nullptr;
    return S_OK;
}

void InputModeLangBarItem::notify_update(DWORD flags) {
    if (sink_) {
        sink_->OnUpdate(flags);
    }
}

HICON InputModeLangBarItem::create_mode_icon() const {
    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return nullptr;
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (!memory_dc) {
        ReleaseDC(nullptr, screen_dc);
        return nullptr;
    }

    BITMAPINFO bitmap_info = {};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = kIconSize;
    bitmap_info.bmiHeader.biHeight = -kIconSize;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color_bitmap = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask_bitmap = CreateBitmap(kIconSize, kIconSize, 1, 1, nullptr);
    if (!color_bitmap || !mask_bitmap) {
        if (color_bitmap) {
            DeleteObject(color_bitmap);
        }
        if (mask_bitmap) {
            DeleteObject(mask_bitmap);
        }
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return nullptr;
    }

    HGDIOBJ old_bitmap = SelectObject(memory_dc, color_bitmap);
    RECT rect = {0, 0, kIconSize, kIconSize};
    FillRect(memory_dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    SetBkMode(memory_dc, TRANSPARENT);
    SetTextColor(memory_dc, RGB(255, 255, 255));

    HFONT font = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = nullptr;
    if (font) {
        old_font = SelectObject(memory_dc, font);
    }

    DrawTextW(memory_dc, mode_label(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    if (bits) {
        auto* pixels = static_cast<unsigned char*>(bits);
        for (int i = 0; i < kIconSize * kIconSize; ++i) {
            unsigned char* pixel = pixels + i * 4;
            const bool text_pixel = pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
            pixel[3] = text_pixel ? 255 : 0;
        }
    }

    if (old_font) {
        SelectObject(memory_dc, old_font);
    }
    if (font) {
        DeleteObject(font);
    }
    SelectObject(memory_dc, old_bitmap);

    ICONINFO icon_info = {};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bitmap;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);

    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return icon;
}

const wchar_t* InputModeLangBarItem::mode_label() const {
    return mode_ == InputMode::Chinese ? L"\x4E2D" : L"\x82F1";
}

const wchar_t* InputModeLangBarItem::tooltip_text() const {
    return mode_ == InputMode::Chinese ? L"Chinese input mode" : L"English input mode";
}

}  // namespace tsf
