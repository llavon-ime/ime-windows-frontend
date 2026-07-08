#pragma once

#include <msctf.h>
#include <winrt/base.h>

#include <functional>

#include "utils/debugSink.hpp"

namespace tsf {

class EditSession : public winrt::implements<EditSession, ITfEditSession> {
public:
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (oper) {
            DebugSink::instance().send(L"INFO", L"EditSession DoEditSession called with operation set");
            oper(ec);
        } else {
            DebugSink::instance().send(L"INFO", L"EditSession DoEditSession called with no operation set");
        }
        return S_OK;
    }

    void set_operation(std::function<void(TfEditCookie)> func) {
        oper = std::move(func);
    }

private:
    std::function<void(TfEditCookie)> oper;
};

}  // namespace tsf
