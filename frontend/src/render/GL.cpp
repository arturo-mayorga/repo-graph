#include "rgv/render/GL.h"

#include <string>

namespace rgv::gl {
namespace {

#define RGV_GL_PTR(name, ret, params, args) ret (*p_##name) params = nullptr;
RGV_GL_FUNCTIONS(RGV_GL_PTR)
#undef RGV_GL_PTR

std::string g_missing;

} // namespace

bool load(ProcLoader loader) {
    g_missing.clear();

    auto resolve = [&](const char* name) -> void* {
        void* p = loader(name);
        if (!p) {
            // Some drivers only expose the ARB-suffixed alias.
            const std::string arb = std::string(name) + "ARB";
            p                     = loader(arb.c_str());
        }
        if (!p) {
            if (!g_missing.empty()) g_missing += ", ";
            g_missing += name;
        }
        return p;
    };

#define RGV_GL_LOAD(name, ret, params, args) \
    p_##name = reinterpret_cast<ret (*) params>(resolve(#name));
    RGV_GL_FUNCTIONS(RGV_GL_LOAD)
#undef RGV_GL_LOAD

    return g_missing.empty();
}

const char* missing() { return g_missing.c_str(); }

#define RGV_GL_DEF(name, ret, params, args) \
    ret name params { return p_##name args; }
RGV_GL_FUNCTIONS(RGV_GL_DEF)
#undef RGV_GL_DEF

} // namespace rgv::gl
