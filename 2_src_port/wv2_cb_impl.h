/* wv2_cb_impl.h - Invoke adapters for the WebView2 callbacks used by the menu. */
#ifndef WV2_CB_IMPL_H
#define WV2_CB_IMPL_H

/* WebView2 environment creation callback. */
template <typename TFn>
class Wv2EnvCb : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    Wv2EnvCb(TFn fn) : fn_(fn), refs_(1) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs_; if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        return fn_(hr, env);
    }
private:
    TFn fn_; ULONG refs_;
};

/* WebView2 controller creation callback. */
template <typename TFn>
class Wv2CtlCb : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    Wv2CtlCb(TFn fn) : fn_(fn), refs_(1) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs_; if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
        return fn_(hr, ctl);
    }
private:
    TFn fn_; ULONG refs_;
};

/* Web message received callback. */
template <typename TFn>
class Wv2MsgCb : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    Wv2MsgCb(TFn fn) : fn_(fn), refs_(1) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs_; if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
            *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* s, ICoreWebView2WebMessageReceivedEventArgs* a) override {
        return fn_(s, a);
    }
private:
    TFn fn_; ULONG refs_;
};

template <typename TFn>
Wv2EnvCb<TFn>* MakeEnvCb(TFn fn)  { return new Wv2EnvCb<TFn>(fn); }
template <typename TFn>
Wv2CtlCb<TFn>* MakeCtlCb(TFn fn)  { return new Wv2CtlCb<TFn>(fn); }
template <typename TFn>
Wv2MsgCb<TFn>* MakeMsgCb(TFn fn)  { return new Wv2MsgCb<TFn>(fn); }

#endif
