#include "pdf_view/text_input_accessibility.h"

#include <oleacc.h>
#include <oleauto.h>
#include <atomic>
#include <limits>
#include <new>

namespace text_input_accessibility {
namespace {

// The standard MSAA-to-UIA proxy can consume this role/value contract as well.
// No application state is owned here: each operation revalidates its edit
// generation on the UI thread before reading or changing the live editor.
class AccessibleTextInput final : public IAccessible {
public:
    AccessibleTextInput(HWND window, std::uint64_t generation, Read read, Modify modify,
                        IAccessible* standard) noexcept
        : window_(window), generation_(generation), read_(read), modify_(modify), standard_(standard) {}
    ~AccessibleTextInput() { standard_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IAccessible) return E_NOINTERFACE;
        *result = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        return standard_->GetTypeInfoCount(count);
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT index, LCID locale, ITypeInfo** info) override {
        return standard_->GetTypeInfo(index, locale, info);
    }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID iid, LPOLESTR* names, UINT count,
                                            LCID locale, DISPID* ids) override {
        return standard_->GetIDsOfNames(iid, names, count, locale, ids);
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID iid, LCID locale, WORD flags,
        DISPPARAMS* parameters, VARIANT* result, EXCEPINFO* exception, UINT* error) override {
        if (iid != IID_NULL) return DISP_E_UNKNOWNINTERFACE;
        ITypeInfo* info = nullptr;
        const HRESULT obtained = GetTypeInfo(0, locale, &info);
        if (FAILED(obtained)) return obtained;
        const HRESULT invoked = DispInvoke(static_cast<IAccessible*>(this), info, id, flags,
                                          parameters, result, exception, error);
        info->Release();
        return invoked;
    }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
        return standard_->get_accParent(parent);
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        Snapshot state;
        return ReadState(&state);
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** child) override {
        if (!child) return E_POINTER;
        *child = nullptr;
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* value) override {
        return ReadString(child, value, true);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
        return ReadString(child, value, false);
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        VariantInit(role);
        Snapshot state;
        const HRESULT status = ReadSelf(child, &state);
        if (FAILED(status)) return status;
        role->vt = VT_I4; role->lVal = ROLE_SYSTEM_TEXT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* value) override {
        if (!value) return E_POINTER;
        VariantInit(value);
        Snapshot state;
        const HRESULT status = ReadSelf(child, &state);
        if (FAILED(status)) return status;
        value->vt = VT_I4;
        value->lVal = STATE_SYSTEM_FOCUSABLE |
            (state.focused ? STATE_SYSTEM_FOCUSED : 0) |
            (state.readOnly ? STATE_SYSTEM_READONLY : 0) |
            (state.enabled ? 0 : STATE_SYSTEM_UNAVAILABLE) |
            (state.offscreen ? STATE_SYSTEM_OFFSCREEN : 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override {
        if (!focus) return E_POINTER;
        VariantInit(focus);
        Snapshot state;
        const HRESULT status = ReadState(&state);
        if (FAILED(status)) return status;
        if (state.focused) { focus->vt = VT_I4; focus->lVal = CHILDID_SELF; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override {
        if (!selection) return E_POINTER;
        VariantInit(selection); // MSAA child selection is not a text/caret range.
        Snapshot state;
        const HRESULT status = ReadState(&state);
        return FAILED(status) ? status : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accLocation(LONG* x, LONG* y, LONG* width, LONG* height,
                                          VARIANT child) override {
        if (!x || !y || !width || !height) return E_POINTER;
        *x = *y = *width = *height = 0;
        Snapshot state;
        const HRESULT status = ReadSelf(child, &state);
        if (FAILED(status)) return status;
        *x = state.screenBounds.left; *y = state.screenBounds.top;
        *width = state.screenBounds.right - state.screenBounds.left;
        *height = state.screenBounds.bottom - state.screenBounds.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(LONG x, LONG y, VARIANT* hit) override {
        if (!hit) return E_POINTER;
        VariantInit(hit);
        Snapshot state;
        const HRESULT status = ReadState(&state);
        if (FAILED(status)) return status;
        if (!state.offscreen && PtInRect(&state.screenBounds, POINT{x, y})) {
            hit->vt = VT_I4; hit->lVal = CHILDID_SELF;
            return S_OK;
        }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accSelect(LONG flags, VARIANT child) override {
        if (!Self(child) || flags != SELFLAG_TAKEFOCUS) return E_INVALIDARG;
        return ModifyState(Action::Focus, nullptr);
    }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR value) override {
        if (!Self(child) || !value) return E_INVALIDARG;
        try {
            std::wstring replacement(value, SysStringLen(value));
            if (replacement.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
            return ModifyState(Action::ReplaceValue, &replacement);
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
          catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE accNavigate(LONG direction, VARIANT start, VARIANT* end) override {
        if (!end) return E_POINTER;
        VariantInit(end);
        Snapshot state;
        const HRESULT status = ReadSelf(start, &state);
        if (FAILED(status)) return status;
        return standard_->accNavigate(direction, start, end);
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* value) override { return NoString(child, value); }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child, BSTR* value) override { return NoString(child, value); }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child, BSTR* value) override { return NoString(child, value); }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* value) override { return NoString(child, value); }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT child, LONG* topic) override {
        if (!topic) return E_POINTER;
        *topic = 0;
        return NoString(child, file);
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        return Self(child) ? E_NOTIMPL : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT child, BSTR) override {
        return Self(child) ? E_NOTIMPL : E_INVALIDARG;
    }

private:
    static bool Self(VARIANT child) noexcept { return child.vt == VT_I4 && child.lVal == CHILDID_SELF; }
    HRESULT CheckThread() const noexcept {
        const DWORD owner = GetWindowThreadProcessId(window_, nullptr);
        if (!owner) return CO_E_OBJNOTCONNECTED;
        return owner == GetCurrentThreadId() ? S_OK : RPC_E_WRONG_THREAD;
    }
    HRESULT ReadState(Snapshot* state) const noexcept {
        const HRESULT status = CheckThread();
        return FAILED(status) ? status : read_(window_, generation_, state);
    }
    HRESULT ReadSelf(VARIANT child, Snapshot* state) const noexcept {
        return Self(child) ? ReadState(state) : E_INVALIDARG;
    }
    HRESULT ModifyState(Action action, const std::wstring* value) noexcept {
        const HRESULT status = CheckThread();
        return FAILED(status) ? status : modify_(window_, generation_, action, value);
    }
    HRESULT ReadString(VARIANT child, BSTR* value, bool name) const noexcept {
        if (!value) return E_POINTER;
        *value = nullptr;
        Snapshot state;
        const HRESULT status = ReadSelf(child, &state);
        if (FAILED(status)) return status;
        const auto& text = name ? state.name : state.value;
        if (text.size() > std::numeric_limits<UINT>::max()) return E_OUTOFMEMORY;
        *value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT NoString(VARIANT child, BSTR* value) const noexcept {
        if (!value) return E_POINTER;
        *value = nullptr;
        Snapshot state;
        const HRESULT status = ReadSelf(child, &state);
        return FAILED(status) ? status : S_FALSE;
    }
    std::atomic<ULONG> references_{1};
    HWND window_;
    std::uint64_t generation_;
    Read read_;
    Modify modify_;
    IAccessible* standard_;
};

} // namespace

LRESULT Expose(HWND window, WPARAM request, std::uint64_t generation, Read read, Modify modify) noexcept {
    if (!window || !read || !modify || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return 0;
    IAccessible* standard = nullptr;
    if (FAILED(CreateStdAccessibleObject(window, OBJID_CLIENT, IID_IAccessible,
                                         reinterpret_cast<void**>(&standard)))) return 0;
    auto* provider = new (std::nothrow) AccessibleTextInput(window, generation, read, modify, standard);
    if (!provider) { standard->Release(); return 0; }
    const LRESULT result = LresultFromObject(IID_IAccessible, request, provider);
    provider->Release();
    return result;
}

} // namespace text_input_accessibility
