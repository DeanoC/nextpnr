// Read-only Quartus 17.0.2 scale observer. Compile with the vendor's old string ABI.
// Every scale argument is forwarded unchanged. Native model getters name its PVT.
// The model/body offsets are diagnostic-only and bound to libtsm_tis.so's hash.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>

extern "C" void audit_native_scale_models(void *, void *, float, float, float, bool)
    asm("_ZN21TIS_ADVANCED_SPICE_RC13set_body_infoEP16TIS_MANAGER_IMPLfffb");

extern "C" void audit_native_scale_models(void *object, void *manager,
                                         float first, float second, float third, bool mode)
{
    using Setter = void (*)(void *, void *, float, float, float, bool);
    using Model = int (*)(int);
    using Name = std::string (*)(int, int, bool);
    const auto original = reinterpret_cast<Setter>(dlsym(
        RTLD_NEXT, "_ZN21TIS_ADVANCED_SPICE_RC13set_body_infoEP16TIS_MANAGER_IMPLfffb"));
    const auto loaded = reinterpret_cast<Model>(dlsym(
        RTLD_DEFAULT, "_ZN12DMGR_UTILITY28get_loaded_timing_model_enumE17dat_timing_models"));
    const auto name = reinterpret_cast<Name>(dlsym(
        RTLD_DEFAULT, "_ZN12DMGR_UTILITY35get_speed_grade_timing_model_stringE17dat_timing_models21DEV_TIMING_MODEL_ENUMb"));
    if (!original || !loaded || !name) std::abort();
    original(object, manager, first, second, third, mode);
    void *body;
    int model;
    std::memcpy(&body, static_cast<const unsigned char *>(object) + 0x10, sizeof(body));
    std::memcpy(&model, static_cast<const unsigned char *>(body) + 0x1c, sizeof(model));
    const auto pvt = name(model, loaded(model), false);
    std::fprintf(stderr, "NATIVE_MODEL_SCALE model=%d pvt=%s first=%.9g second=%.9g third=%.9g mode=%d\n",
                 model, pvt.c_str(), double(first), double(second), double(third), int(mode));
}
