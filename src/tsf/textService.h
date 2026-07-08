#pragma once

#include <msctf.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/buffer.hpp"

namespace tsf {

class CandidateUiController;

// clang-format off
class TextService : 
    public winrt::implements<
        TextService,
        ITfTextInputProcessor,
        ITfTextInputProcessorEx, 
        ITfThreadMgrEventSink,
        ITfKeyEventSink, 
        ITfCompositionSink, 
        ITfDisplayAttributeProvider> {
    // clang-format on
public:
    TextService();
    ~TextService();

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) override;
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* pThreadMgr, TfClientId tfClientId, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus, ITfDocumentMgr* pDocMgrPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pContext) override;
    STDMETHODIMP OnPopContext(ITfContext* pContext) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

private:
    HRESULT activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId);
    void deactivate();

    HRESULT start_composition(ITfContext* pContext);
    HRESULT end_composition(ITfContext* pContext);
    HRESULT discard_composition(ITfContext* pContext);
    HRESULT insert_text(ITfContext* pContext, const std::u16string& text);
    HRESULT set_composition_text(ITfContext* pContext, const std::u16string& text,
                                 size_t select_start = std::u16string::npos, size_t select_length = 0);
    void refresh_composition_after_candidate_finalize(ITfContext* pContext);
    void show_candidate_list_for_current_input(ITfContext* pContext, bool expand);
    void show_candidate_list(BopomofoPos& pos, ITfContext* pContext);
    std::u16string get_pre_composit_context(ITfContext* pContext);

    winrt::com_ptr<ITfThreadMgr> threadMgr;
    TfClientId _tfClientId = TF_CLIENTID_NULL;
    DWORD dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    winrt::com_ptr<ITfComposition> itfComposition;
    std::unique_ptr<CandidateUiController> candidate_ui_;
    CompositionBuffer compositionBuffer;
    bool shift_toggle_pending_ = false;
    bool shift_used_as_modifier_ = false;
};

};  // namespace tsf
