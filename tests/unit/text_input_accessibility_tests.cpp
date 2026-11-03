#include "pdf_view/text_input_accessibility.h"
#include <oleacc.h>
#include <oleauto.h>
#include <iostream>
#include <stdexcept>

namespace {
bool active = false;
bool readOnly = false;
std::uint64_t generation = 1;
std::wstring text = L"Annotation \u6ce8\u91c8";
int writes = 0;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
HRESULT Read(HWND, std::uint64_t expected, text_input_accessibility::Snapshot* out) noexcept {
    if (!active || expected != generation) return CO_E_OBJNOTCONNECTED;
    try {
        out->name = L"Annotation text input";
        out->value = text;
        out->screenBounds = {10, 20, 210, 60};
        out->enabled = out->focused = true;
        out->readOnly = readOnly;
        out->offscreen = false;
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT Modify(HWND, std::uint64_t expected, text_input_accessibility::Action action,
               const std::wstring* value) noexcept {
    if (!active || expected != generation) return CO_E_OBJNOTCONNECTED;
    if (action == text_input_accessibility::Action::Focus) return S_OK;
    if (readOnly) return E_ACCESSDENIED;
    try { text = *value; ++writes; return S_OK; }
    catch (...) { return E_OUTOFMEMORY; }
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_GETOBJECT && static_cast<LONG>(l) == static_cast<LONG>(OBJID_CLIENT) && active)
        return text_input_accessibility::Expose(window, w, generation, Read, Modify);
    return DefWindowProcW(window, message, w, l);
}
IAccessible* Accessible(HWND window) {
    IAccessible* value = nullptr;
    Require(SUCCEEDED(AccessibleObjectFromWindow(window, OBJID_CLIENT, IID_IAccessible,
        reinterpret_cast<void**>(&value))) && value, "WM_GETOBJECT did not publish an accessible object");
    return value;
}
VARIANT Self() { VARIANT value{}; value.vt = VT_I4; value.lVal = CHILDID_SELF; return value; }
}

int main() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int result = 0;
    try {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"TextInputAccessibilityContractFixture";
        type.lpfnWndProc = WindowProc;
        Require(RegisterClassW(&type) != 0, "Could not register the hidden fixture");
        HWND window = CreateWindowW(type.lpszClassName, L"", WS_POPUP, 0, 0, 300, 100,
                                    nullptr, nullptr, type.hInstance, nullptr);
        Require(window != nullptr && !IsWindowVisible(window), "Fixture must stay hidden");
        active = true;
        IAccessible* input = Accessible(window);
        VARIANT role{}, state{}, focus{};
        Require(input->get_accRole(Self(), &role) == S_OK && role.vt == VT_I4 &&
                role.lVal == ROLE_SYSTEM_TEXT, "Input role was not editable text");
        Require(input->get_accState(Self(), &state) == S_OK &&
                (state.lVal & (STATE_SYSTEM_FOCUSED | STATE_SYSTEM_FOCUSABLE)) ==
                    (STATE_SYSTEM_FOCUSED | STATE_SYSTEM_FOCUSABLE) &&
                !(state.lVal & STATE_SYSTEM_READONLY), "Writable focus state was incorrect");
        Require(input->get_accFocus(&focus) == S_OK && focus.vt == VT_I4 &&
                focus.lVal == CHILDID_SELF, "Focused input was not exposed");
        BSTR value = nullptr;
        Require(input->get_accValue(Self(), &value) == S_OK &&
                std::wstring(value, SysStringLen(value)) == text, "Unicode value was not preserved");
        SysFreeString(value);
        LONG x = 0, y = 0, width = 0, height = 0;
        Require(input->accLocation(&x, &y, &width, &height, Self()) == S_OK &&
                x == 10 && y == 20 && width == 200 && height == 40, "Input bounds were incorrect");
        VARIANT hit{};
        Require(input->accHitTest(20, 30, &hit) == S_OK && hit.vt == VT_I4 &&
                hit.lVal == CHILDID_SELF, "Input hit-test failed");
        Require(input->accHitTest(400, 400, &hit) == S_FALSE && hit.vt == VT_EMPTY,
                "Outside point was misreported as an input hit");
        value = SysAllocString(L"Changed \u65e5\u672c\u8a9e");
        Require(value && input->put_accValue(Self(), value) == S_OK && writes == 1,
                "Writable value did not reach the owner");
        readOnly = true;
        Require(input->get_accState(Self(), &state) == S_OK && (state.lVal & STATE_SYSTEM_READONLY),
                "Readonly state was not exposed");
        Require(input->put_accValue(Self(), value) == E_ACCESSDENIED && writes == 1,
                "Readonly update changed the value");
        SysFreeString(value);
        readOnly = false;
        value = SysAllocStringLen(L"a\0b", 3);
        Require(value && input->put_accValue(Self(), value) == E_INVALIDARG && writes == 1,
                "Embedded NUL was silently truncated");
        SysFreeString(value);
        VARIANT invalid{};
        Require(input->get_accRole(invalid, &role) == E_INVALIDARG, "Invalid child was accepted");
        // IDispatch must invoke the overridden value, not the standard HWND proxy.
        OLECHAR property[] = L"accValue";
        LPOLESTR names[] = {property};
        DISPID id = 0;
        Require(input->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id) == S_OK,
                "Value dispatch metadata was missing");
        VARIANT child = Self(), dispatched{};
        DISPPARAMS parameters{&child, nullptr, 1, 0};
        Require(input->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
            &parameters, &dispatched, nullptr, nullptr) == S_OK && dispatched.vt == VT_BSTR &&
            std::wstring(dispatched.bstrVal, SysStringLen(dispatched.bstrVal)) == text,
            "IDispatch bypassed the live input value");
        VariantClear(&dispatched);
        ++generation;
        Require(input->get_accValue(Self(), &value) == CO_E_OBJNOTCONNECTED && !value,
                "Old provider exposed a new edit session");
        value = SysAllocString(L"stale");
        Require(value && input->put_accValue(Self(), value) == CO_E_OBJNOTCONNECTED && writes == 1,
                "Old provider changed a new edit session");
        SysFreeString(value);
        input->Release();
        input = Accessible(window);
        Require(input->get_accValue(Self(), &value) == S_OK, "New edit session was unavailable");
        SysFreeString(value);
        active = false;
        Require(input->get_accRole(Self(), &role) == CO_E_OBJNOTCONNECTED,
                "Ended session retained an input role");
        DestroyWindow(window);
        Require(input->get_accValue(Self(), &value) == CO_E_OBJNOTCONNECTED,
                "Destroyed window retained an input value");
        input->Release();
        std::cout << "PASS: text input accessibility role/value/focus/bounds/dispatch/readonly/lifetime contracts; hidden fixture.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
