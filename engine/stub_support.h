// Support code for the generated engine stubs (see tools/gen_engine_stubs.py).
#ifndef SHOGO_ENGINE_STUB_SUPPORT_H
#define SHOGO_ENGINE_STUB_SUPPORT_H

#include "engine/api.h"

#include <type_traits>

// Records that an unimplemented engine function was called.
void shogo_stub_called(const char *name);
#define SHOGO_STUB_CALLED(name) shogo_stub_called(name)

namespace shogo_stub {

extern const char *const kFunctionNames[];
extern const int kFunctionCount;
extern const char *const kVirtualNames[];
extern const int kVirtualCount;
extern void *g_vhooks[];  // non-null entries replace the stub for that virtual method

// Lets a type spelled as text (e.g. a function-pointer type) be used in a declarator.
template <class T> using T_ = T;

// A zero/empty value of any return type. References refer to a zeroed static object.
template <class R> R zero()
{
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_reference_v<R>) {
        static std::remove_reference_t<R> value{};
        return value;
    } else {
        return R{};
    }
}

// Stub for a function-pointer table entry: logs its name, returns zero.
template <class F, int ID> struct Fn;
template <class R, class... A, int ID> struct Fn<R (*)(A...), ID> {
    static R call(A...) { shogo_stub_called(kFunctionNames[ID]); return zero<R>(); }
};
template <class R, class... A, int ID> struct Fn<R (*)(A..., ...), ID> {
    static R call(A..., ...) { shogo_stub_called(kFunctionNames[ID]); return zero<R>(); }
};

}  // namespace shogo_stub

// Replace a virtual method ("CSBase::WriteToMessageFloat", "ServerDE::GetPropGeneric"...) on
// both stub engines. fn's first parameter is the engine object (void*), then the method's own.
bool shogo_set_virtual_hook(const char *name, void *fn);
ClientDE *shogo_create_stub_client();
ServerDE *shogo_create_stub_server();

#endif
