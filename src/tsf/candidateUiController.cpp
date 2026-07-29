#include "candidateUiController.hpp"

#include "editSession.hpp"
#include "utils/debugSink.hpp"

namespace tsf {
namespace {

bool is_current_process_app_container() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD is_app_container = 0;
    DWORD returned_size = 0;
    const BOOL result = GetTokenInformation(
        token, TokenIsAppContainer, &is_app_container, sizeof(is_app_container), &returned_size);
    CloseHandle(token);
    return result != FALSE && is_app_container != 0;
}

bool use_uwp_xaml_popup(ITfThreadMgr* thread_mgr) noexcept {
    DWORD active_flags = 0;
    winrt::com_ptr<ITfThreadMgrEx> thread_mgr_ex;
    if (thread_mgr != nullptr &&
        SUCCEEDED(thread_mgr->QueryInterface<ITfThreadMgrEx>(thread_mgr_ex.put())) &&
        SUCCEEDED(thread_mgr_ex->GetActiveFlags(&active_flags)) &&
        (active_flags & TF_TMF_IMMERSIVEMODE) != 0) {
        return true;
    }

    // The token check is a safety guard: never try DesktopWindowXamlSource from
    // an AppContainer even if an older TSF host omitted IMMERSIVEMODE.
    return is_current_process_app_container();
}

}  // namespace

void CandidateUiController::attach(ITfThreadMgr* thread_mgr, TfClientId client_id) {
    thread_mgr_.copy_from(thread_mgr);
    client_id_ = client_id;
    const bool use_uwp_popup = use_uwp_xaml_popup(thread_mgr);
    element_->set_uwp_xaml_popup(use_uwp_popup);
    DebugSink::instance().send(
        L"INFO", L"CandidateUiController::attach backend=" +
                     std::wstring(use_uwp_popup ? L"UWP XAML Popup" : L"Desktop XAML Island"));
}

void CandidateUiController::detach() {
    hide();
    thread_mgr_ = nullptr;
    client_id_ = TF_CLIENTID_NULL;
}

bool CandidateUiController::is_active() const {
    return element_ && element_->is_shown();
}

bool CandidateUiController::can_handle_key(WPARAM wParam) const {
    return is_active() && element_->can_handle_key(wParam);
}

CandidateKeyResult CandidateUiController::handle_key(WPARAM wParam) {
    if (!is_active()) {
        return CandidateKeyResult::not_handled;
    }

    DebugSink::instance().send(L"INFO", L"CandidateUiController::handle_key key=" + std::to_wstring(wParam));

    const CandidateKeyResult result = element_->handle_key(wParam);
    if (result == CandidateKeyResult::aborted || result == CandidateKeyResult::not_handled) {
        hide();
    }
    return result;
}

void CandidateUiController::show(ITfContext* context, const std::vector<std::wstring>& candidates,
                                 std::function<void(std::wstring)> on_finalize) {
    if (!context || candidates.empty()) {
        hide();
        return;
    }

    winrt::com_ptr<ITfUIElementMgr> ui_element_mgr = get_ui_element_mgr();
    if (!ui_element_mgr) {
        return;
    }

    HWND owner_window = nullptr;
    query_owner_window(context, &owner_window);
    element_->set_owner_window(owner_window);

    POINT anchor = {};
    if (query_anchor(context, &anchor)) {
        element_->set_anchor_point(anchor);
    } else {
        element_->clear_anchor_point();
    }

    element_->update(candidates, [this, callback = std::move(on_finalize)](std::wstring word) mutable {
        if (callback) {
            callback(std::move(word));
        }
        dismiss_ui_element();
    });

    if (is_active()) {
        ui_element_mgr->UpdateUIElement(ui_element_id_);
        element_->Show(TRUE);
        return;
    }

    BOOL should_show = TRUE;
    const HRESULT hr = ui_element_mgr->BeginUIElement(element_.get(), &should_show, &ui_element_id_);
    if (FAILED(hr)) {
        ui_element_id_ = TF_INVALID_COOKIE;
        return;
    }

    if (should_show) {
        element_->Show(TRUE);
    }
}

void CandidateUiController::expand() {
    if (element_) {
        element_->expand();
    }
}

void CandidateUiController::hide() {
    dismiss_ui_element();
}

winrt::com_ptr<ITfUIElementMgr> CandidateUiController::get_ui_element_mgr() const {
    if (!thread_mgr_) {
        return nullptr;
    }

    winrt::com_ptr<ITfUIElementMgr> ui_element_mgr;
    if (FAILED(thread_mgr_->QueryInterface<ITfUIElementMgr>(ui_element_mgr.put()))) {
        return nullptr;
    }
    return ui_element_mgr;
}

bool CandidateUiController::query_owner_window(ITfContext* context, HWND* owner_window) const {
    if (!context || !owner_window) {
        return false;
    }

    *owner_window = nullptr;

    winrt::com_ptr<ITfContextView> context_view;
    const HRESULT hr_view = context->GetActiveView(context_view.put());
    if (SUCCEEDED(hr_view) && context_view) {
        HWND context_window = nullptr;
        if (SUCCEEDED(context_view->GetWnd(&context_window)) && context_window != nullptr) {
            *owner_window = context_window;
            return true;
        }
    }

    HWND focus_window = GetFocus();
    if (focus_window == nullptr) {
        return false;
    }

    *owner_window = focus_window;
    return true;
}

bool CandidateUiController::query_anchor(ITfContext* context, POINT* anchor) const {
    if (!context || !anchor || client_id_ == TF_CLIENTID_NULL) {
        return false;
    }

    winrt::com_ptr<ITfContextView> context_view;
    HRESULT hr = context->GetActiveView(context_view.put());
    if (FAILED(hr) || !context_view) {
        return false;
    }

    POINT point = {};
    bool found = false;
    winrt::com_ptr<EditSession> edit_session = winrt::make_self<EditSession>();
    edit_session->set_operation([context, context_view, &point, &found](TfEditCookie ec) {
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        const HRESULT hr_selection = context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (FAILED(hr_selection) || fetched == 0 || !selection.range) {
            return;
        }

        winrt::com_ptr<ITfRange> range;
        range.attach(selection.range);

        RECT rc = {};
        BOOL clipped = FALSE;
        const HRESULT hr_text_ext = context_view->GetTextExt(ec, range.get(), &rc, &clipped);
        if (FAILED(hr_text_ext)) {
            return;
        }

        point.x = rc.left;
        point.y = rc.bottom;
        found = true;
    });

    HRESULT hr_session = E_FAIL;
    hr = context->RequestEditSession(client_id_, edit_session.get(), TF_ES_READ | TF_ES_SYNC, &hr_session);
    if (FAILED(hr) || FAILED(hr_session) || !found) {
        return false;
    }

    *anchor = point;
    return true;
}

void CandidateUiController::dismiss_ui_element() {
    if (!element_) {
        return;
    }

    element_->Show(FALSE);
    element_->clear_anchor_point();

    if (ui_element_id_ == TF_INVALID_COOKIE) {
        return;
    }

    winrt::com_ptr<ITfUIElementMgr> ui_element_mgr = get_ui_element_mgr();
    if (ui_element_mgr) {
        ui_element_mgr->EndUIElement(ui_element_id_);
    }
    ui_element_id_ = TF_INVALID_COOKIE;
}

}  // namespace tsf
