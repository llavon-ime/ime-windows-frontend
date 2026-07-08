#include "textService.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>

#include "candidateUiController.hpp"
#include "core/bopomofo.hpp"
#include "editSession.hpp"
#include "engine/engine.hpp"
#include "system/globals.h"
#include "utils/debugSink.hpp"
#include "utils/healper.hpp"

using namespace std::literals;

namespace {

inline constexpr winrt::guid kCompositionDisplayAttributeGuid = {
    0x82769d2d, 0x9e5d, 0x4ace, {0x97, 0x53, 0x91, 0x3c, 0x0c, 0x7c, 0x4e, 0x38}};

TF_DISPLAYATTRIBUTE make_composition_display_attribute() {
    TF_DISPLAYATTRIBUTE attribute = {};
    attribute.crText.type = TF_CT_NONE;
    attribute.crBk.type = TF_CT_NONE;
    attribute.lsStyle = TF_LS_DOT;
    attribute.fBoldLine = FALSE;
    attribute.crLine.type = TF_CT_SYSCOLOR;
    attribute.crLine.nIndex = COLOR_WINDOWTEXT;
    attribute.bAttr = TF_ATTR_INPUT;
    return attribute;
}

class CompositionDisplayAttributeInfo
    : public winrt::implements<CompositionDisplayAttributeInfo, ITfDisplayAttributeInfo> {
public:
    CompositionDisplayAttributeInfo() : attribute_(make_composition_display_attribute()), default_(attribute_) {}

    STDMETHODIMP GetGUID(GUID* pguid) override {
        if (!pguid) {
            return E_INVALIDARG;
        }
        *pguid = kCompositionDisplayAttributeGuid;
        return S_OK;
    }

    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override {
        if (!pbstrDesc) {
            return E_INVALIDARG;
        }
        *pbstrDesc = SysAllocString(L"拉風輸入法組字");
        return (*pbstrDesc != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) {
            return E_INVALIDARG;
        }
        *pda = attribute_;
        return S_OK;
    }

    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) {
            return E_INVALIDARG;
        }
        attribute_ = *pda;
        return S_OK;
    }

    STDMETHODIMP Reset() override {
        attribute_ = default_;
        return S_OK;
    }

private:
    TF_DISPLAYATTRIBUTE attribute_ = {};
    TF_DISPLAYATTRIBUTE default_ = {};
};

class DisplayAttributeEnum : public winrt::implements<DisplayAttributeEnum, IEnumTfDisplayAttributeInfo> {
public:
    explicit DisplayAttributeEnum(winrt::com_ptr<ITfDisplayAttributeInfo> info, ULONG index = 0)
        : info_(std::move(info)), index_(index) {}

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) {
            return E_INVALIDARG;
        }
        *ppEnum = nullptr;
        auto enumerator = winrt::make_self<DisplayAttributeEnum>(info_, index_);
        enumerator.as<IEnumTfDisplayAttributeInfo>().copy_to(ppEnum);
        return S_OK;
    }

    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override {
        if (!rgInfo) {
            return E_INVALIDARG;
        }
        if (ulCount != 1 && !pcFetched) {
            return E_INVALIDARG;
        }

        ULONG fetched = 0;
        while (fetched < ulCount && index_ == 0 && info_) {
            rgInfo[fetched] = info_.get();
            rgInfo[fetched]->AddRef();
            ++fetched;
            ++index_;
        }

        if (pcFetched) {
            *pcFetched = fetched;
        }

        return (fetched == ulCount) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }

    STDMETHODIMP Skip(ULONG ulCount) override {
        if (ulCount == 0) {
            return S_OK;
        }
        if (index_ == 0) {
            index_ = 1;
            return (ulCount == 1) ? S_OK : S_FALSE;
        }
        return S_FALSE;
    }

private:
    winrt::com_ptr<ITfDisplayAttributeInfo> info_;
    ULONG index_ = 0;
};

HRESULT get_composition_display_attribute_atom(TfGuidAtom* atom) {
    if (!atom) {
        return E_INVALIDARG;
    }

    static TfGuidAtom cached_atom = TF_INVALID_GUIDATOM;
    if (cached_atom != TF_INVALID_GUIDATOM) {
        *atom = cached_atom;
        return S_OK;
    }

    ITfCategoryMgr* raw_category_mgr = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                        reinterpret_cast<void**>(&raw_category_mgr));
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfCategoryMgr> category_mgr;
    category_mgr.attach(raw_category_mgr);

    const HRESULT register_hr = category_mgr->RegisterGUID(kCompositionDisplayAttributeGuid, &cached_atom);
    if (FAILED(register_hr)) {
        return register_hr;
    }

    *atom = cached_atom;
    return S_OK;
}

HRESULT apply_composition_display_attribute(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!context || !range) {
        return E_INVALIDARG;
    }

    TfGuidAtom atom = TF_INVALID_GUIDATOM;
    HRESULT hr = get_composition_display_attribute_atom(&atom);
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfProperty> attribute_property;
    hr = context->GetProperty(GUID_PROP_ATTRIBUTE, attribute_property.put());
    if (FAILED(hr)) {
        return hr;
    }

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = atom;
    return attribute_property->SetValue(ec, range, &value);
}

bool key_down(int virtual_key) {
    return (GetKeyState(virtual_key) & 0x8000) != 0 || (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool shift_key(WPARAM wParam) {
    return wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT;
}

bool modifier_key(WPARAM wParam) {
    return shift_key(wParam) || wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
           wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU;
}

bool modified_passthrough_key(WPARAM wParam) {
    if (modifier_key(wParam)) {
        return true;
    }

    return key_down(VK_CONTROL) || key_down(VK_MENU) || key_down(VK_SHIFT);
}

bool english_printable_key(WPARAM wParam) {
    if (modifier_key(wParam) || key_down(VK_CONTROL) || key_down(VK_MENU)) {
        return false;
    }

    if (wParam == VK_SPACE || (wParam >= '0' && wParam <= '9') || (wParam >= 'A' && wParam <= 'Z') ||
        (wParam >= VK_NUMPAD0 && wParam <= VK_NUMPAD9)) {
        return true;
    }

    switch (wParam) {
        case VK_OEM_1:
        case VK_OEM_PLUS:
        case VK_OEM_COMMA:
        case VK_OEM_MINUS:
        case VK_OEM_PERIOD:
        case VK_OEM_2:
        case VK_OEM_3:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
        case VK_OEM_8:
        case VK_OEM_102:
        case VK_MULTIPLY:
        case VK_ADD:
        case VK_SEPARATOR:
        case VK_SUBTRACT:
        case VK_DECIMAL:
        case VK_DIVIDE:
            return true;
        default:
            return false;
    }
}

std::optional<std::u16string> printable_key_text(WPARAM wParam, LPARAM lParam) {
    if (!english_printable_key(wParam)) {
        return std::nullopt;
    }

    BYTE keyboard_state[256] = {};
    if (!GetKeyboardState(keyboard_state)) {
        return std::nullopt;
    }

    std::array<WCHAR, 8> chars = {};
    UINT scan_code = static_cast<UINT>((lParam >> 16) & 0xff);
    if (scan_code == 0) {
        scan_code = MapVirtualKeyW(static_cast<UINT>(wParam), MAPVK_VK_TO_VSC);
    }

    constexpr UINT no_keyboard_state_change = 0x4;
    const int count = ToUnicode(static_cast<UINT>(wParam), scan_code, keyboard_state, chars.data(),
                                static_cast<int>(chars.size()), no_keyboard_state_change);
    if (count <= 0) {
        return std::nullopt;
    }

    std::u16string text;
    const int safe_count = std::min(count, static_cast<int>(chars.size()));
    for (int i = 0; i < safe_count; i++) {
        const WCHAR ch = chars[i];
        if (std::iswcntrl(ch)) {
            return std::nullopt;
        }
        text.push_back(static_cast<char16_t>(ch));
    }
    return text.empty() ? std::nullopt : std::optional{text};
}

std::optional<std::u16string> punctuation_shortcut(WPARAM wParam) {
    if (!key_down(VK_CONTROL) || key_down(VK_MENU) || modifier_key(wParam)) {
        return std::nullopt;
    }

    if (key_down(VK_SHIFT)) {
        switch (wParam) {
            case VK_OEM_COMMA:
                return u"\u300A";
            case VK_OEM_PERIOD:
                return u"\u300B";
            case VK_OEM_1:
                return u"\uFF1A";
            case VK_OEM_7:
                return u"\uFF02";
            case VK_OEM_4:
                return u"\uFF5B";
            case VK_OEM_6:
                return u"\uFF5D";
            case '1':
                return u"\uFF01";
            case VK_OEM_2:
                return u"\uFF1F";
            default:
                return std::nullopt;
        }
    }

    switch (wParam) {
        case VK_OEM_COMMA:
            return u"\uFF0C";
        case VK_OEM_PERIOD:
            return u"\u3002";
        case VK_OEM_1:
            return u"\uFF1B";
        case VK_OEM_7:
            return u"\u3001";
        case VK_OEM_2:
            return u"\u2026";
        case VK_OEM_MINUS:
            return u"\u2014";
        case VK_OEM_4:
            return u"\u3010";
        case VK_OEM_6:
            return u"\u3011";
        default:
            return std::nullopt;
    }
}

}  // namespace

namespace tsf {

TextService::TextService() : candidate_ui_(std::make_unique<CandidateUiController>()) {}

TextService::~TextService() = default;

/**
 * @brief Implements ITfTextInputProcessor::Activate.
 *
 * Delegates activation to the shared setup path.
 */
STDMETHODIMP TextService::Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) try {
    return activate(pThreadMgr, tfClientId);
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfTextInputProcessor::Deactivate.
 *
 * Tears down TSF state through the shared cleanup path.
 */
STDMETHODIMP TextService::Deactivate() try {
    deactivate();
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfTextInputProcessorEx::ActivateEx.
 *
 * Uses the same activation flow and currently ignores extra flags.
 */
STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* pThreadMgr, TfClientId tfClientId, DWORD /*dwFlags*/) try {
    return activate(pThreadMgr, tfClientId);
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Shared activation helper.
 *
 * Attaches TSF sinks, stores thread manager state, and starts debug logging.
 */
HRESULT TextService::activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) {
    if (!pThreadMgr) return E_INVALIDARG;

    threadMgr.copy_from(pThreadMgr);
    _tfClientId = tfClientId;
    candidate_ui_->attach(pThreadMgr, tfClientId);

    winrt::com_ptr<ITfSource> itfSource;
    HRESULT hr = threadMgr->QueryInterface<ITfSource>(itfSource.put());
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    hr = itfSource->AdviseSink(
        IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &dwThreadMgrEventSinkCookie);
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    winrt::com_ptr<ITfKeystrokeMgr> itfKeystrokeMgr;
    hr = threadMgr->QueryInterface<ITfKeystrokeMgr>(itfKeystrokeMgr.put());
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    hr = itfKeystrokeMgr->AdviseKeyEventSink(tfClientId, static_cast<ITfKeyEventSink*>(this), TRUE);
    if (FAILED(hr)) {
        deactivate();
        return hr;
    }

    DebugSink::instance().connect();
    DebugSink::instance().send(L"IME", L"Activated");

    return S_OK;
}

/**
 * @brief Shared deactivation helper.
 *
 * Releases TSF sinks, clears composition state, and stops debug logging.
 */
void TextService::deactivate() {
    DebugSink::instance().send(L"IME", L"Deactivated");
    candidate_ui_->hide();
    DebugSink::instance().disconnect();

    if (itfComposition) {
        itfComposition->EndComposition(TF_INVALID_COOKIE);
        itfComposition = nullptr;
    }
    compositionBuffer.clear();

    if (threadMgr) {
        winrt::com_ptr<ITfKeystrokeMgr> itfKeystrokeMgr;
        if (SUCCEEDED(threadMgr->QueryInterface<ITfKeystrokeMgr>(itfKeystrokeMgr.put()))) {
            itfKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
        }

        if (dwThreadMgrEventSinkCookie != TF_INVALID_COOKIE) {
            winrt::com_ptr<ITfSource> pSource;
            if (SUCCEEDED(threadMgr->QueryInterface(IID_PPV_ARGS(pSource.put())))) {
                pSource->UnadviseSink(dwThreadMgrEventSinkCookie);
            }
            dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
        }

        threadMgr = nullptr;
    }
    _tfClientId = TF_CLIENTID_NULL;
    candidate_ui_->detach();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnInitDocumentMgr.
 *
 * No document-manager initialization is required yet.
 */
STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr* /*pDocMgr*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnUninitDocumentMgr.
 *
 * No document-manager teardown is required yet.
 */
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* /*pDocMgr*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnSetFocus.
 *
 * Receives document focus changes but does not react to them yet.
 */
STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* /*pDocMgrFocus*/, ITfDocumentMgr* /*pDocMgrPrevFocus*/) try {
    // TODO: Initialize or clear per-document state on focus switch.
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnPushContext.
 *
 * Accepts new contexts without additional bookkeeping.
 */
STDMETHODIMP TextService::OnPushContext(ITfContext* /*pContext*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfThreadMgrEventSink::OnPopContext.
 *
 * Releases contexts without additional cleanup.
 */
STDMETHODIMP TextService::OnPopContext(ITfContext* /*pContext*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnSetFocus.
 *
 * Tracks foreground changes but currently keeps no extra state.
 */
STDMETHODIMP TextService::OnSetFocus(BOOL /*fForeground*/) try { return S_OK; } catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnTestKeyDown.
 *
 * Reports whether the service intends to consume the key-down event.
 */
STDMETHODIMP TextService::OnTestKeyDown(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    DebugSink::instance().send(L"EVENT", L"OnTestKeyDown key=" + std::to_wstring(wParam));

    if (shift_key(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (key_down(VK_SHIFT)) {
        shift_toggle_pending_ = false;
        shift_used_as_modifier_ = true;
    }

    const bool english_mode = get_engine()->current_input_mode() == InputMode::English;
    if (english_mode && compositionBuffer.empty()) {
        *pfEaten = (punctuation_shortcut(wParam) || english_printable_key(wParam)) ? TRUE : FALSE;
        return S_OK;
    }

    if (punctuation_shortcut(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (english_mode && english_printable_key(wParam)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (modified_passthrough_key(wParam)) {
        *pfEaten = FALSE;
        return S_OK;
    }

    if (candidate_ui_->is_active()) {
        const bool backspace_composition = (wParam == VK_BACK && !compositionBuffer.empty());
        *pfEaten = (candidate_ui_->can_handle_key(wParam) || backspace_composition) ? TRUE : FALSE;
        DebugSink::instance().send(
            L"EVENT", L"OnTestKeyDown candidate mode, eaten="s + (*pfEaten ? L"TRUE" : L"FALSE"));
        return S_OK;
    }

    if (!compositionBuffer.empty()) {
        const bool editing_key = (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_BACK ||
                                  wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_DOWN ||
                                  wParam == VK_SPACE);
        if (editing_key) {
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    const bool is_bopomofo_key = Bopomofo::lookup(static_cast<int>(wParam)) != std::nullopt;
    const bool starts_composition = is_bopomofo_key && wParam != VK_SPACE;
    *pfEaten = starts_composition ? TRUE : FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnTestKeyUp.
 *
 * Leaves key-up events unhandled by default.
 */
STDMETHODIMP TextService::OnTestKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = shift_key(wParam) && shift_toggle_pending_ ? TRUE : FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnKeyDown.
 *
 * Updates the active composition or handles commit and cancel keys.
 */
STDMETHODIMP TextService::OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    DebugSink::instance().send(L"EVENT", L"OnKeyDown");

    if (shift_key(wParam)) {
        const bool repeated_keydown = (lParam & (1LL << 30)) != 0;
        if (!repeated_keydown) {
            shift_toggle_pending_ = true;
            shift_used_as_modifier_ = false;
        }
        *pfEaten = TRUE;
        return S_OK;
    }

    if (key_down(VK_SHIFT)) {
        shift_toggle_pending_ = false;
        shift_used_as_modifier_ = true;
    }

    const bool english_mode = get_engine()->current_input_mode() == InputMode::English;
    if (english_mode && compositionBuffer.empty()) {
        if (const auto punctuation = punctuation_shortcut(wParam)) {
            insert_text(pContext, *punctuation);
            *pfEaten = TRUE;
            return S_OK;
        }

        if (const auto text = printable_key_text(wParam, lParam)) {
            insert_text(pContext, *text);
            *pfEaten = TRUE;
            return S_OK;
        }

        return S_OK;
    }

    if (const auto punctuation = punctuation_shortcut(wParam)) {
        compositionBuffer.add_chosen_candidate((*punctuation)[0]);
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (english_mode) {
        if (const auto text = printable_key_text(wParam, lParam)) {
            candidate_ui_->hide();
            for (const char16_t ch : *text) {
                compositionBuffer.add_chosen_candidate(ch);
            }
            set_composition_text(pContext, compositionBuffer.to_string());
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    if (modified_passthrough_key(wParam)) {
        return S_OK;
    }

    if (candidate_ui_->is_active()) {
        const CandidateKeyResult result = candidate_ui_->handle_key(wParam);
        switch (result) {
            case CandidateKeyResult::navigated:
                *pfEaten = TRUE;
                return S_OK;
            case CandidateKeyResult::finalized:
                refresh_composition_after_candidate_finalize(pContext);
                *pfEaten = TRUE;
                return S_OK;
            case CandidateKeyResult::aborted:
                *pfEaten = TRUE;
                return S_OK;
            case CandidateKeyResult::not_handled:
            default:
                break;
        }
    }

    if (wParam == VK_RETURN && !compositionBuffer.empty()) {
        DebugSink::instance().send(L"COMMIT", compositionBuffer.to_string());
        end_composition(pContext);
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_ESCAPE && itfComposition) {
        DebugSink::instance().send(L"CANCEL", compositionBuffer.to_string());
        discard_composition(pContext);
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_BACK && !compositionBuffer.empty()) {
        if (!compositionBuffer.remove_last()) {
            candidate_ui_->hide();
            *pfEaten = TRUE;
            return S_OK;
        }
        if (compositionBuffer.empty()) {
            discard_composition(pContext);
        } else {
            set_composition_text(pContext, compositionBuffer.to_string());
        }
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_DOWN && !compositionBuffer.empty()) {
        show_candidate_list_for_current_input(pContext, false);
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_LEFT && !compositionBuffer.empty()) {
        candidate_ui_->hide();
        compositionBuffer.pre();
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_RIGHT && !compositionBuffer.empty()) {
        candidate_ui_->hide();
        compositionBuffer.next();
        set_composition_text(pContext, compositionBuffer.to_string());
        *pfEaten = TRUE;
        return S_OK;
    }

    if (wParam == VK_SPACE && !compositionBuffer.empty() && compositionBuffer.current_compositable()) {
        end_composition(pContext);
        insert_text(pContext, u" ");
        *pfEaten = TRUE;
        return S_OK;
    }

    auto cur_char = Bopomofo::lookup(static_cast<int>(wParam));
    if (cur_char == std::nullopt || (wParam == VK_SPACE && compositionBuffer.empty())) {
        candidate_ui_->hide();
        // TODO: Handle non-Bopomofo keys.
        *pfEaten = FALSE;
        return S_OK;
    }

    if (wParam != VK_SPACE) {
        get_engine()->ready();
    }

    // if (!itfComposition) {
    //     start_composition(pContext);
    // }
    compositionBuffer.add(cur_char.value());
    const auto invalid_span = compositionBuffer.current_invalid_span();
    if (!invalid_span) {
        compositionBuffer.predict_paddings(get_pre_composit_context(pContext));
    }
    DebugSink::instance().send(L"KEY", compositionBuffer.to_string());
    if (invalid_span) {
        set_composition_text(pContext, compositionBuffer.to_string(), invalid_span->first, invalid_span->second);
    } else {
        set_composition_text(pContext, compositionBuffer.to_string());
    }
    *pfEaten = TRUE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnKeyUp.
 *
 * Leaves key-up events unconsumed after key-down handling.
 */
STDMETHODIMP TextService::OnKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    if (shift_key(wParam) && shift_toggle_pending_) {
        if (!shift_used_as_modifier_) {
            const InputMode mode = get_engine()->toggle_input_mode();
            DebugSink::instance().send(L"MODE", mode == InputMode::Chinese ? L"Chinese" : L"English");
        }

        shift_toggle_pending_ = false;
        shift_used_as_modifier_ = false;
        *pfEaten = TRUE;
        return S_OK;
    }

    *pfEaten = FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfKeyEventSink::OnPreservedKey.
 *
 * Declines preserved-key handling because no preserved keys are registered.
 */
STDMETHODIMP TextService::OnPreservedKey(ITfContext* /*pContext*/, REFGUID /*rguid*/, BOOL* pfEaten) try {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfCompositionSink::OnCompositionTerminated.
 *
 * Clears local composition state when TSF ends the composition externally.
 */
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/, ITfComposition* /*pComposition*/) try {
    candidate_ui_->hide();
    itfComposition = nullptr;
    compositionBuffer.clear();
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfDisplayAttributeProvider::EnumDisplayAttributeInfo.
 *
 * Returns not implemented because display attributes are not exposed yet.
 */
STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) try {
    if (!ppEnum) {
        return E_INVALIDARG;
    }

    *ppEnum = nullptr;
    auto info = winrt::make_self<CompositionDisplayAttributeInfo>();
    auto enumerator = winrt::make_self<DisplayAttributeEnum>(info.as<ITfDisplayAttributeInfo>());
    enumerator.as<IEnumTfDisplayAttributeInfo>().copy_to(ppEnum);
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Implements ITfDisplayAttributeProvider::GetDisplayAttributeInfo.
 *
 * Returns not implemented because no display attribute metadata is defined yet.
 */
STDMETHODIMP_(HRESULT __stdcall)
TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) try {
    if (!ppInfo) {
        return E_INVALIDARG;
    }

    *ppInfo = nullptr;
    if (!IsEqualGUID(guid, kCompositionDisplayAttributeGuid)) {
        return E_INVALIDARG;
    }

    auto info = winrt::make_self<CompositionDisplayAttributeInfo>();
    info.as<ITfDisplayAttributeInfo>().copy_to(ppInfo);
    return S_OK;
} catch (...) {
    return handle_com_exception();
}

/**
 * @brief Starts a new TSF composition session.
 *
 * Creates the TSF composition objects needed for a new input session.
 */
HRESULT TextService::start_composition(ITfContext* pContext) {
    if (!pContext) return E_INVALIDARG;
    if (itfComposition) return S_OK;

    winrt::com_ptr<ITfContextComposition> contextComposition;
    HRESULT hr = pContext->QueryInterface<ITfContextComposition>(contextComposition.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
    hr = pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();

    editSession->set_operation([this, contextComposition, insertAtSelection](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> range;
        if (FAILED(insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, L"", 0, range.put()))) {
            return;
        }
        if (FAILED(contextComposition->StartComposition(
                ec, range.get(), static_cast<ITfCompositionSink*>(this), itfComposition.put()))) {
            return;
        }
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

/**
 * @brief Ends the active TSF composition session.
 *
 * Clears the current composition object and buffered text.
 */
HRESULT TextService::end_composition(ITfContext* pContext) {
    candidate_ui_->hide();
    if (!itfComposition) return S_OK;

    const std::u16string text = compositionBuffer.to_string();
    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([this, pContext, text](TfEditCookie ec) {
        if (itfComposition) {
            winrt::com_ptr<ITfRange> range;
            itfComposition->GetRange(range.put()) | win::check();
            range->SetText(ec, 0, convu16(text.data()), static_cast<LONG>(text.size())) | win::check();

            winrt::com_ptr<ITfRange> caret_range;
            range->Clone(caret_range.put()) | win::check();
            caret_range->Collapse(ec, TF_ANCHOR_END) | win::check();

            itfComposition->EndComposition(ec) | win::check();
            itfComposition = nullptr;

            TF_SELECTION selection = {};
            selection.range = caret_range.get();
            selection.style.ase = TF_AE_END;
            selection.style.fInterimChar = FALSE;
            pContext->SetSelection(ec, 1, &selection) | win::check();

            compositionBuffer.clear();
        }
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

HRESULT TextService::discard_composition(ITfContext* pContext) {
    candidate_ui_->hide();
    compositionBuffer.clear();
    if (!pContext) return E_INVALIDARG;
    if (!itfComposition) return S_OK;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([this, pContext](TfEditCookie ec) {
        if (!itfComposition) {
            return;
        }

        winrt::com_ptr<ITfRange> range;
        itfComposition->GetRange(range.put()) | win::check();

        winrt::com_ptr<ITfRange> caret_range;
        range->Clone(caret_range.put()) | win::check();
        caret_range->Collapse(ec, TF_ANCHOR_START) | win::check();

        range->SetText(ec, 0, L"", 0) | win::check();
        itfComposition->EndComposition(ec) | win::check();
        itfComposition = nullptr;

        TF_SELECTION selection = {};
        selection.range = caret_range.get();
        selection.style.ase = TF_AE_END;
        selection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &selection) | win::check();
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

HRESULT TextService::insert_text(ITfContext* pContext, const std::u16string& text) {
    if (!pContext) return E_INVALIDARG;
    if (text.empty()) return S_OK;

    winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([pContext, insertAtSelection, text](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> range;
        insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.put()) | win::check();
        range->SetText(ec, 0, convu16(text.data()), static_cast<LONG>(text.size())) | win::check();

        winrt::com_ptr<ITfRange> caret_range;
        range->Clone(caret_range.put()) | win::check();
        caret_range->Collapse(ec, TF_ANCHOR_END) | win::check();

        TF_SELECTION selection = {};
        selection.range = caret_range.get();
        selection.style.ase = TF_AE_END;
        selection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &selection) | win::check();
    });

    HRESULT hrSession;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hrSession) |
        win::check();

    return S_OK;
}

/**
 * @brief Updates the active composition text.
 *
 * Applies the visible composition string to the current TSF context.
 */
HRESULT TextService::set_composition_text(ITfContext* pContext, const std::u16string& text, size_t select_start,
                                          size_t select_length) {
    if (!pContext) return E_INVALIDARG;

    winrt::com_ptr<ITfContextComposition> contextComposition;
    HRESULT hr = pContext->QueryInterface<ITfContextComposition>(contextComposition.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<ITfInsertAtSelection> insertAtSelection;
    hr = pContext->QueryInterface<ITfInsertAtSelection>(insertAtSelection.put());
    if (FAILED(hr)) return hr;

    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([=, this](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> range;
        if (!itfComposition) {
            insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.put()) | win::check();
            contextComposition->StartComposition(
                ec, range.get(), static_cast<ITfCompositionSink*>(this), itfComposition.put()) |
                win::check();
        }

        range = nullptr;
        itfComposition->GetRange(range.put()) | win::check();
        range->SetText(ec, 0, convu16(text.data()), ULONG(text.size())) | win::check();

        apply_composition_display_attribute(pContext, ec, range.get()) | win::check();

        const bool select_span = select_start != std::u16string::npos && select_length > 0;
        winrt::com_ptr<ITfRange> selection_range;
        range->Clone(selection_range.put()) | win::check();
        if (select_span) {
            const LONG start = static_cast<LONG>(select_start);
            const LONG length = static_cast<LONG>(select_length);
            LONG shifted = 0;
            selection_range->Collapse(ec, TF_ANCHOR_START) | win::check();
            selection_range->ShiftEnd(ec, start + length, &shifted, nullptr) | win::check();
            selection_range->ShiftStart(ec, start, &shifted, nullptr) | win::check();
        } else {
            const LONG caret = static_cast<LONG>(std::min(compositionBuffer.caret_offset(), text.size()));
            LONG shifted = 0;
            selection_range->Collapse(ec, TF_ANCHOR_START) | win::check();
            selection_range->ShiftEnd(ec, caret, &shifted, nullptr) | win::check();
            selection_range->Collapse(ec, TF_ANCHOR_END) | win::check();
        }

        TF_SELECTION selection = {};
        selection.range = selection_range.get();
        selection.style.ase = select_span ? TF_AE_NONE : TF_AE_END;
        selection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &selection) | win::check();
    });
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READWRITE | TF_ES_SYNC, &hr) | win::check();
    return S_OK;
}

void TextService::refresh_composition_after_candidate_finalize(ITfContext* pContext) {
    if (!pContext || compositionBuffer.empty()) {
        return;
    }

    DebugSink::instance().send(
        L"INFO", L"refresh_composition_after_candidate_finalize text="_u16 + compositionBuffer.to_string());
    compositionBuffer.invalidate_all_predictions();
    compositionBuffer.predict_paddings(get_pre_composit_context(pContext));
    set_composition_text(pContext, compositionBuffer.to_string());
}

void TextService::show_candidate_list_for_current_input(ITfContext* pContext, bool expand) {
    if (!pContext || compositionBuffer.empty() || !compositionBuffer.current_has_candidate_list()) {
        candidate_ui_->hide();
        return;
    }

    auto& target = compositionBuffer.candidate_target();
    show_candidate_list(target, pContext);
    if (expand) {
        candidate_ui_->expand();
    }
}

/**
 * @brief Displays the candidate list UI with the given candidates.
 */
void TextService::show_candidate_list(BopomofoPos& bopomofoPos, ITfContext* pContext) {
    // TODO
    std::shared_ptr<std::vector<std::wstring>> candidate_ptr = std::make_shared<std::vector<std::wstring>>();
    for (auto candi : bopomofoPos.get_candidates()) {
        std::u16string str;
        utf8::append16(candi, str);
        std::wstring wstr(str.begin(), str.end());
        candidate_ptr->push_back(wstr);
    }

    candidate_ui_->show(pContext, *candidate_ptr, [&bopomofoPos, candidate_ptr](std::wstring word) {
        auto& candidate = *candidate_ptr;
        for (int i = 0; i < candidate.size(); i++) {
            if (word == candidate[i]) {
                bopomofoPos.set_choose_index(i);
                return;
            }
        }
    });
}

inline constexpr LONG kMaxPreCompositionContextChars = 256;
inline constexpr ULONG kRangeReadChunkChars = 128;

bool append_range_text(TfEditCookie ec, ITfRange* range, std::u16string& context) {
    if (!range) {
        return false;
    }

    std::array<WCHAR, kRangeReadChunkChars> chunk = {};
    while (true) {
        ULONG copied = 0;
        const HRESULT hr = range->GetText(ec, TF_TF_MOVESTART, chunk.data(), static_cast<ULONG>(chunk.size()), &copied);
        if (FAILED(hr)) {
            return false;
        }
        if (copied == 0) {
            return true;
        }

        context.append(reinterpret_cast<const char16_t*>(chunk.data()), copied);
        if (copied < chunk.size()) {
            return true;
        }
    }
}

bool read_pre_context_with_acp(TfEditCookie ec, ITfRange* anchor_range, std::u16string& context) {
    if (!anchor_range) {
        return false;
    }

    winrt::com_ptr<ITfRangeACP> anchor_acp;
    if (FAILED(anchor_range->QueryInterface(IID_PPV_ARGS(anchor_acp.put())))) {
        return false;
    }

    LONG anchor_start = 0;
    LONG anchor_length = 0;
    if (FAILED(anchor_acp->GetExtent(&anchor_start, &anchor_length))) {
        return false;
    }
    if (anchor_start <= 0) {
        return true;
    }

    const LONG start = std::max<LONG>(0, anchor_start - kMaxPreCompositionContextChars);
    const LONG length = anchor_start - start;
    if (length <= 0) {
        return true;
    }

    winrt::com_ptr<ITfRange> read_range;
    if (FAILED(anchor_range->Clone(read_range.put())) || !read_range) {
        return false;
    }

    winrt::com_ptr<ITfRangeACP> read_acp;
    if (FAILED(read_range->QueryInterface(IID_PPV_ARGS(read_acp.put())))) {
        return false;
    }
    if (FAILED(read_acp->SetExtent(start, length))) {
        return false;
    }

    context.clear();
    return append_range_text(ec, read_range.get(), context);
}

bool read_pre_context_with_shift(TfEditCookie ec, ITfRange* anchor_range, std::u16string& context) {
    if (!anchor_range) {
        return false;
    }

    winrt::com_ptr<ITfRange> read_range;
    if (FAILED(anchor_range->Clone(read_range.put())) || !read_range) {
        return false;
    }

    LONG shifted = 0;
    if (FAILED(read_range->ShiftStart(ec, -kMaxPreCompositionContextChars, &shifted, nullptr))) {
        return false;
    }

    context.clear();
    return append_range_text(ec, read_range.get(), context);
}

std::u16string TextService::get_pre_composit_context(ITfContext* pContext) {
    if (!pContext) {
        return {};
    }

    std::u16string context;
    winrt::com_ptr<EditSession> editSession = winrt::make_self<EditSession>();
    editSession->set_operation([this, pContext, &context](TfEditCookie ec) {
        winrt::com_ptr<ITfRange> anchor_range;

        if (itfComposition) {
            itfComposition->GetRange(anchor_range.put()) | win::check();
        } else {
            TF_SELECTION selection = {};
            ULONG fetched = 0;
            pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched) | win::check();
            if (fetched == 0 || !selection.range) {
                return;
            }

            anchor_range.attach(selection.range);
        }

        anchor_range->Collapse(ec, TF_ANCHOR_START) | win::check();
        if (!read_pre_context_with_acp(ec, anchor_range.get(), context)) {
            read_pre_context_with_shift(ec, anchor_range.get(), context);
        }
    });

    HRESULT hr = S_OK;
    pContext->RequestEditSession(_tfClientId, editSession.get(), TF_ES_READ | TF_ES_SYNC, &hr) | win::check();
    return context;
}

}  // namespace tsf
