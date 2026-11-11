/* wv2_cb.h - Small COM callback adapters without WRL or wil.
 * Each adapter stores a callable and maintains an IUnknown reference count.
 * See wv2_cb_impl.h for WebView2-specific Invoke implementations.
 */
#ifndef WV2_CB_H
#define WV2_CB_H
#include <windows.h>
#include <functional>

template <typename TInterface, typename TFn>
class Wv2Callback : public TInterface {
public:
    Wv2Callback(TFn fn) : fn_(fn), refs_(1) {}

    /* IUnknown lifetime and interface queries. */
    ULONG STDMETHODCALLTYPE AddRef() override   { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override  {
        ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(TInterface)) {
            *ppv = static_cast<TInterface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    /* The specialized Invoke implementation forwards arguments to TFn. */
    template <typename... Args>
    HRESULT invoke(Args... args) { return fn_(args...); }

private:
    TFn   fn_;
    ULONG refs_;
};

/* Concrete WebView2 callback adapters are defined in wv2_cb_impl.h. */
#endif
