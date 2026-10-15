#include <jni.h>
#include <exception>
#include <new>

#include "android_binder_backend.h"
#include "native_session.h"

// The shared library name and historical JNI names are retained. Device mode
// only diagnoses an open Binder connection through the official BINDER_VERSION
// request. Simulation is entirely local. No exploit or credential change exists.
namespace {
diagnostics::AndroidBinderBackend backend;
diagnostics::Session session(backend);

void throwJava(JNIEnv* env, const char* className, const char* message) noexcept {
    if (env->ExceptionCheck()) return;
    jclass exceptionClass = env->FindClass(className);
    if (exceptionClass != nullptr) {
        env->ThrowNew(exceptionClass, message);
        env->DeleteLocalRef(exceptionClass);
    }
}

template <typename Result, typename Function>
Result guarded(JNIEnv* env, Result failure, Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        throwJava(env, "java/lang/OutOfMemoryError", "Native diagnostic memory allocation failed");
    } catch (const std::exception& error) {
        throwJava(env, "java/lang/IllegalStateException", error.what());
    } catch (...) {
        throwJava(env, "java/lang/IllegalStateException", "Unexpected native diagnostic failure");
    }
    return failure;
}

template <typename Function>
void guardedVoid(JNIEnv* env, Function&& function) noexcept {
    guarded<int>(env, 0, [&] { function(); return 0; });
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_simulation_NativeCore_status(JNIEnv* env, jobject) {
    return guarded<jstring>(env, nullptr, [&] { return env->NewStringUTF(session.status().c_str()); });
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_simulation_NativeCore_getProcInfo(JNIEnv* env, jobject) {
    return guarded<jlong>(env, 0, [&] { return static_cast<jlong>(session.procInfo()); });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_executeBadSpin(JNIEnv* env, jobject, jlong target) {
    return guarded<jboolean>(env, JNI_FALSE, [&] { return session.execute(target) ? JNI_TRUE : JNI_FALSE; });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_checkSystemHealth(JNIEnv* env, jobject) {
    return guarded<jboolean>(env, JNI_FALSE, [&] { return session.checkSimulationHealth() ? JNI_TRUE : JNI_FALSE; });
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_simulation_NativeCore_resetKernelState(JNIEnv* env, jobject) {
    guardedVoid(env, [&] { session.reset(); });
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_simulation_NativeCore_getDetailedProcInfo(JNIEnv* env, jobject) {
    return guarded<jstring>(env, nullptr, [&] { return env->NewStringUTF(session.detailedState().c_str()); });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_initProductionMode(JNIEnv* env, jobject) {
    return guarded<jboolean>(env, JNI_FALSE, [&] { return session.initializeDevice() ? JNI_TRUE : JNI_FALSE; });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_elevateToRoot(JNIEnv* env, jobject) {
    return guarded<jboolean>(env, JNI_FALSE, [&] { return session.elevateToRoot() ? JNI_TRUE : JNI_FALSE; });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_isProductionMode(JNIEnv* env, jobject) {
    return guarded<jboolean>(env, JNI_FALSE, [&] { return session.isDeviceMode() ? JNI_TRUE : JNI_FALSE; });
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_simulation_NativeCore_closeDevice(JNIEnv* env, jobject) {
    guardedVoid(env, [&] { session.closeDevice(); });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_probeDevice(JNIEnv* env, jobject) {
    return guarded<jboolean>(env, JNI_FALSE, [&] { return session.probeDevice() ? JNI_TRUE : JNI_FALSE; });
}

// Public Kotlin methods hold the shared workflow monitor before invoking these
// private native aliases. Legacy names above remain exported for compatibility.
#define JNI_ALIAS_NO_ARGS(type, alias, legacy) \
extern "C" JNIEXPORT type JNICALL Java_com_example_simulation_NativeCore_##alias(JNIEnv* env, jobject instance) { \
    return Java_com_example_simulation_NativeCore_##legacy(env, instance); \
}

JNI_ALIAS_NO_ARGS(jstring, nativeStatus, status)
JNI_ALIAS_NO_ARGS(jlong, nativeGetProcInfo, getProcInfo)
JNI_ALIAS_NO_ARGS(jboolean, nativeCheckSystemHealth, checkSystemHealth)
JNI_ALIAS_NO_ARGS(void, nativeResetKernelState, resetKernelState)
JNI_ALIAS_NO_ARGS(jstring, nativeGetDetailedProcInfo, getDetailedProcInfo)
JNI_ALIAS_NO_ARGS(jboolean, nativeInitProductionMode, initProductionMode)
JNI_ALIAS_NO_ARGS(jboolean, nativeElevateToRoot, elevateToRoot)
JNI_ALIAS_NO_ARGS(jboolean, nativeIsProductionMode, isProductionMode)
JNI_ALIAS_NO_ARGS(void, nativeCloseDevice, closeDevice)
JNI_ALIAS_NO_ARGS(jboolean, nativeProbeDevice, probeDevice)

#undef JNI_ALIAS_NO_ARGS

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_simulation_NativeCore_nativeExecuteBadSpin(JNIEnv* env, jobject instance, jlong target) {
    return Java_com_example_simulation_NativeCore_executeBadSpin(env, instance, target);
}
