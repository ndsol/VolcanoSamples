/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * JNI methods (only used for Android).
 */

#include <src/science/science-glfw.h>
#include "retrojni.h"
#ifdef __ANDROID__
#include <android_native_app_glue.h>
#include <jni.h>
#include <math.h>
#include <stdarg.h>


static jmethodID rjGetMethod(JNIEnv* env, jclass c, const char* name,
                                  const char* args) {
  if (!env) {
    logE("rjGetMethod(%s): env is NULL", name);
    return NULL;
  }
  if (!c) {
    logE("rjGetMethod(%s): jclass is NULL", name);
    return NULL;
  }
  jmethodID r = (*env)->GetMethodID(env, c, name, args);
  if (!r) {
    logE("GetMethodID(%s) returned NULL", name);
  }
  return r;
}

static void rjDestroy(ANativeActivity* activity, int mustDetach)
{
  if (mustDetach) {
    jint result = (*activity->vm)->DetachCurrentThread(activity->vm);
    if (result != JNI_OK) {
      logE("rjDestroy: DetachCurrentThread failed: %lld",
           (long long)result);
    }
  }
}

#define EXCEPTION_STRLEN (256)
// rjGetString enforces EXCEPTION_STRLEN on 'out'.
static int rjGetString(JNIEnv* env, jobject o, char* out, jmethodID m) {
  if (!env) {
    logE("rjGetString: env is NULL");
    return 1;
  }
  if (!o) {
    logE("rjGetString: object is NULL");
    return 1;
  }
  if (!m) {
    logE("rjGetString: methodID is NULL");
    return 1;
  }

  jstring str = (jstring) (*env)->CallObjectMethod(env, o, m);
  if (!str) {
    logE("rjGetString: CallObjectMethod failed");
    return 1;
  }
  jthrowable ex = (*env)->ExceptionOccurred(env);
  if (ex) {
    (*env)->ExceptionClear(env);
    logE("rjGetString: throws exception");
    (*env)->DeleteLocalRef(env, ex);
    return 1;
  }
  jint strlen = (*env)->GetStringUTFLength(env, str);
  if (strlen > EXCEPTION_STRLEN - 1) {
    strlen = EXCEPTION_STRLEN - 1;
  }
  (*env)->GetStringUTFRegion(env, str, 0, strlen, out);
  out[strlen] = 0;
  (*env)->DeleteLocalRef(env, str);
  return 0;
}

static int rjCheckException(JNIEnv* env, char* typeStr) {
  jthrowable ex = (*env)->ExceptionOccurred(env);
  if (!ex) {
    return 0;
  }
  (*env)->ExceptionClear(env);

  jmethodID jgetClass = rjGetMethod(env, (*env)->GetObjectClass(env, ex),
                                    "getClass", "()Ljava/lang/Class;");
  if (!jgetClass) {
    snprintf(typeStr, EXCEPTION_STRLEN, "GetMethodID(%s) returned NULL",
             "getClass");
    return 1;
  }
  jobject o = (*env)->CallObjectMethod(env, ex, jgetClass);
  if (!o) {
    snprintf(typeStr, EXCEPTION_STRLEN, "CallObjectMethod(getClass) failed");
    return 1;
  }
  jthrowable ex2 = (*env)->ExceptionOccurred(env);
  if (ex2) {
    (*env)->ExceptionClear(env);
    snprintf(typeStr, EXCEPTION_STRLEN,
             "getClass of exception also throws exception");
    (*env)->DeleteLocalRef(env, ex2);
    return 1;
  }
  (*env)->DeleteLocalRef(env, ex);
  if (rjGetString(env, o, typeStr,
                  rjGetMethod(env, (*env)->GetObjectClass(env, o), "getName",
                              "()Ljava/lang/String;"))) {
    snprintf(typeStr, EXCEPTION_STRLEN, "rjGetString(exception) failed");
    return 1;
  }
  return 1;
}

static JNIEnv* rjInit(ANativeActivity* activity, int* mustDetach)
{
  *mustDetach = 0;
  JNIEnv* env = NULL;

  // https://developer.android.com/training/articles/perf-jni
  // "In theory you can have multiple JavaVMs per process, but Android only
  // allows one." Thus there is always only one env per thread.
  jint r = (*activity->vm)->GetEnv(activity->vm, (void**)&env,
                                   JNI_VERSION_1_6);
  if (r != JNI_OK && r != JNI_EDETACHED) {
    if (r == JNI_EVERSION) {
      logE("rjInit: GetEnv: v1.6 not supported: JNI_EVERSION");
    } else {
      logE("rjInit: GetEnv failed: %lld", (long long)r);
    }
    return NULL;
  }
  if (r == JNI_OK) {
    if (!env) {
      logE("%s returned NULL", "rjInit: GetEnv succeeded but");
      return NULL;
    }
    return env;
  }

  // This thread is not yet attached. Attach it.
  JavaVMAttachArgs arg;
  arg.version = JNI_VERSION_1_6;
  arg.name = "rjAndroid";  // Shows up in stack traces.
  arg.group = NULL;
  r = (*activity->vm)->AttachCurrentThread(activity->vm, &env, &arg);
  if (r != JNI_OK) {
    logE("AttachCurrentThread failed: %lld", (long long)r);
    return NULL;
  }
  if (!env) {
    logE("AttachCurrentThread failed: env is NULL");
    return NULL;
  }
  char exTypeStr[EXCEPTION_STRLEN];
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", "rjInit", exTypeStr);
    rjDestroy(activity, 1);
    return NULL;
  }
  *mustDetach = 1;
  return env;
}

static int voidCall(JNIEnv* env, jobject o, jmethodID m,
                    const char* methodName, ...) {
  char exTypeStr[EXCEPTION_STRLEN];
  va_list ap;

  if (!o) {
    logE("%s: NULL object", methodName);
    return 1;
  }
  if (!m) {
    logE("getMethod(%s) failed", methodName);
    return 1;
  }

  va_start(ap, methodName);
  (*env)->CallVoidMethodV(env, o, m, ap);
  va_end(ap);
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", methodName,
                    exTypeStr);
    return 1;
  }
  return 0;
}

static jobject objectCall(JNIEnv* env, jobject o, jmethodID m,
                          const char* methodName, ...) {
  char exTypeStr[EXCEPTION_STRLEN];
  va_list ap;
  jobject r;

  if (!o) {
    logE("%s: NULL object", methodName);
    return NULL;
  }
  if (!m) {
    logE("getMethod(%s) failed", methodName);
    return NULL;
  }

  va_start(ap, methodName);
  if (!strncmp(methodName, "new(", 4)) {
    r = (*env)->NewObjectV(env, o, m, ap);
  } else {
    r = (*env)->CallObjectMethodV(env, o, m, ap);
  }
  va_end(ap);
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", methodName,
                    exTypeStr);
    return NULL;
  }
  if (!r) {
    logE("%s returned NULL", methodName);
  }
  return r;
}

static jobject objectCallQuiet(JNIEnv* env, jobject o, jmethodID m,
                               const char* methodName, ...) {
  char exTypeStr[EXCEPTION_STRLEN];
  va_list ap;
  jobject r;

  if (!o) {
    logE("%s: NULL object", methodName);
    return NULL;
  }
  if (!m) {
    logE("getMethod(%s) failed", methodName);
    return NULL;
  }

  va_start(ap, methodName);
  if (!strncmp(methodName, "new(", 4)) {
    r = (*env)->NewObjectV(env, o, m, ap);
  } else {
    r = (*env)->CallObjectMethodV(env, o, m, ap);
  }
  va_end(ap);
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", methodName,
                    exTypeStr);
    return NULL;
  }
  return r;
}

static int intCall(JNIEnv* env, jobject o, jmethodID m, const char* methodName,
                   ...) {
  char exTypeStr[EXCEPTION_STRLEN];
  va_list ap;

  if (!o) {
    logE("%s: NULL object", methodName);
    return -42;
  }
  if (!m) {
    logE("getMethod(%s) failed", methodName);
    return -42;
  }

  va_start(ap, methodName);
  int r = (*env)->CallIntMethodV(env, o, m, ap);
  va_end(ap);
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", methodName,
                    exTypeStr);
    return -42;
  }
  return r;
}

static int floatCall(JNIEnv* env, jobject o, jmethodID m,
                     const char* methodName, ...) {
  char exTypeStr[EXCEPTION_STRLEN];
  va_list ap;

  if (!o) {
    logE("%s: NULL object", methodName);
    return NAN;
  }
  if (!m) {
    logE("getMethod(%s) failed", methodName);
    return NAN;
  }

  va_start(ap, methodName);
  float r = (*env)->CallFloatMethodV(env, o, m, ap);
  va_end(ap);
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", methodName,
                    exTypeStr);
    return NAN;
  }
  return r;
}

static jobject staticObjectCall(JNIEnv* env, jclass c, jmethodID m,
                                const char* methodName, ...) {
  char exTypeStr[EXCEPTION_STRLEN];
  va_list ap;

  if (!c) {
    logE("%s: FindClass failed", methodName);
    return NULL;
  }
  if (!m) {
    logE("getMethod(%s) failed", methodName);
    return NULL;
  }

  va_start(ap, methodName);
  jobject r = (*env)->CallStaticObjectMethodV(env, c, m, ap);
  va_end(ap);
  if (rjCheckException(env, exTypeStr)) {
    logE("%s exception: %s", methodName,
                    exTypeStr);
    return NULL;
  }
  if (!r) {
    logE("%s returned NULL", methodName);
  }
  return r;
}

static jclass safeFindClass(JNIEnv* env, const char* className)
{
  jclass c = (*env)->FindClass(env, className);
  char exTypeStr[EXCEPTION_STRLEN];
  if (rjCheckException(env, exTypeStr)) {
    logE("FindClass(%s) exception: %s", className, exTypeStr);
    return NULL;
  } else if (!c) {
    logE("FindClass(%s) failed", className);
    return NULL;
  }
  return c;
}


static void _androidSetNeedsMenuKeyInner(
    ANativeActivity* activity, JNIEnv* env, int menuKey)
{
  if (!env) {
    logE("%s: %s failed", "_androidSetNeedsMenuKeyInner", "rjInit");
    return;
  }
  jclass jWindow = safeFindClass(env, "android/view/Window");
  jclass jInteger = safeFindClass(env, "java/lang/Integer");
  jclass jClass = safeFindClass(env, "java/lang/Class");
  jclass jMethod = safeFindClass(env, "java/lang/reflect/Method");
  jclass jIntCl = (*env)->GetStaticObjectField(env, jInteger,
      (*env)->GetStaticFieldID(env, jInteger, "TYPE", "Ljava/lang/Class;"));
  if (!jWindow || !jInteger || !jClass || !jMethod || !jIntCl) {
    logE("%s: Window, Integer or Class failed",
           "_androidSetNeedsMenuKeyInner");
    return;
  }
  jobjectArray ja = (*env)->NewObjectArray(env, 1, jClass, jIntCl);
  // Not needed: SetObjectArrayElement(env, ja, 0, jIntCl);
  // (because NewObjectArray fills the array with jIntCl as the default).
  jstring methodName = (*env)->NewStringUTF(env, "setNeedsMenuKey");

  jmethodID getWindow = rjGetMethod(
      env, (*env)->GetObjectClass(env, activity->clazz), "getWindow",
      "()Landroid/view/Window;");
  jobject window = objectCall(env, activity->clazz, getWindow, "getWindow");
  jmethodID getDecl = (*env)->GetMethodID(env, jClass, "getDeclaredMethod",
      "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
  jmethodID setAccessible = rjGetMethod(env, jMethod, "setAccessible", "(Z)V");
  if (!ja || !methodName || !window || !getDecl || !setAccessible) {
    logE("%s: ja or window or getDeclaredMethod or setAccessible failed",
         "_androidSetNeedsMenuKeyInner");
    return;
  }

  jobject set = objectCall(env, jWindow, getDecl, "getDeclaredMethod",
      methodName, ja);
  if (voidCall(env, set, setAccessible, "setAccessible", (jboolean)1)) {
    logE("%s: set.setAccessible(true) failed",
         "_androidSetNeedsMenuKeyInner");
    return;
  }
  jmethodID invoke = (*env)->FromReflectedMethod(env, set);
  if (!invoke) {
    logE("%s: invoke FromReflectedMethod failed",
         "_androidSetNeedsMenuKeyInner");
    return;
  }
  (*env)->CallVoidMethod(env, window, invoke, menuKey);
  char exTypeStr[EXCEPTION_STRLEN];
  if (rjCheckException(env, exTypeStr)) {
    logE("%s: setNeedsMenuKey(%d) failed: %s",
         "_androidSetNeedsMenuKeyInner", menuKey, exTypeStr);
  }
  (*env)->DeleteLocalRef(env, set);
  (*env)->DeleteLocalRef(env, window);
  (*env)->DeleteLocalRef(env, methodName);
  (*env)->DeleteLocalRef(env, ja);
  (*env)->DeleteLocalRef(env, jIntCl);
  (*env)->DeleteLocalRef(env, jClass);
  (*env)->DeleteLocalRef(env, jInteger);
  (*env)->DeleteLocalRef(env, jWindow);
}

void androidSetNeedsMenuKey(int menuKey)
{
  struct android_app* app = glfwGetAndroidApp();
  // Show the Overflow Action Menu, a.k.a. "the menu button of shame."
  // https://android-developers.googleblog.com/2012/01/say-goodbye-to-menu-button.html
  // https://gist.github.com/jgilfelt/6812732

  // Before API 14, the overflow action menu is always available.
  // API 14 - 20, just set a window flag:
  // #define FLAG_NEEDS_MENU_KEY (0x08000000)
  // ANativeActivity_setWindowFlags(activity, FLAG_NEEDS_MENU_KEY, 0);
  //
  // But after Android 4 (API 20), reflection must be used:
  int mustDetach;
  JNIEnv* env = rjInit(app->activity, &mustDetach);
  _androidSetNeedsMenuKeyInner(app->activity, env, menuKey);
  rjDestroy(app->activity, mustDetach);
}

#if 0
static void _androidRetroRuntimeResultCB(jint requestCode, jobject permArray,
                                         jobject grantResultArray) {
  logE("_androidRetroRuntimeResultCB: requestCode=%d\n", (int)requestCode);
}

static JNINativeMethod _retroNatives[] = {
  {"onRequestPermissionsResult", "(I[Ljava/lang/String;[I)V",
    (void*)&_androidRetroRuntimeResultCB},
};
  jclass jNativeActivity = safeFindClass(env, "android/app/NativeActivity");
  if (!jNativeActivity) {
    logE("NativeActivity not found\n");
    return 1;
  }
  logE("got class NativeActivity ok, call RegisterNatives\n");
  if ((*env)->RegisterNatives(
      env, jNativeActivity, _retroNatives,
      sizeof(_retroNatives)/sizeof(_retroNatives[0])) < 0) {
    char exTypeStr[EXCEPTION_STRLEN];
    if (rjCheckException(env, exTypeStr)) {
      logE("RegisterNatives exception: %s", exTypeStr);
    }
    logE("RegisterNatives failed\n");
    return 1;
  }
  logE("RegisterNatives done\n");
#endif

static int _androidRetroRuntimePerms(ANativeActivity* activity, JNIEnv* env) {
  if (!env) {
    logE("%s: %s failed", "_androidRetroRuntimePerms", "rjInit");
    return 1;
  }

  jclass jContext = safeFindClass(env, "android/content/Context");
  jclass jFile = safeFindClass(env, "java/io/File");
  if (!jContext || !jFile) {
    logE("getClass(%s) failed", "java.io.File or android.context.Context");
    char exTypeStr[EXCEPTION_STRLEN];
    if (rjCheckException(env, exTypeStr)) {
      logE("getClass() exception: %s", exTypeStr);
    }
    return 1;
  }

  if (0) {
    // This code works, but Android Q deprecates public external storage.
    // *Maybe* allow the user to request storage at a different path, a path
    // the user has to manually type in. And if the user requests that, then
    // ask for permission to read/write to external storage.
    //
    // NOTE: requestPermissions has a callback - that would require java source
    // code. As a hack, as long as the app stays alive while requestPermissions
    // is over it (i.e. do not exit with a fatal error), Android will then exit
    // and restart the app when the requestPermissions fragment popup is done.
    // On restart, checkSelfPermission returns PERMISSION_GRANTED.
    jclass jActivity = safeFindClass(env, "android/app/Activity");
    jclass jPM = safeFindClass(env, "android/content/pm/PackageManager");
    jmethodID perm = rjGetMethod(
        env, jContext, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!perm || !jPM) {
      logE("getMethod(%s) failed", "checkSelfPermission");
      char exTypeStr[EXCEPTION_STRLEN];
      if (rjCheckException(env, exTypeStr)) {
        logE("getMethod(%s) exception: %s", "checkSelfPermission", exTypeStr);
      }
      return 1;
    }
    jmethodID req = rjGetMethod(env, jActivity, "requestPermissions",
                                "([Ljava/lang/String;I)V");
    if (!req) {
      logE("getMethod(%s) failed", "requestPermissions");
      char exTypeStr[EXCEPTION_STRLEN];
      if (rjCheckException(env, exTypeStr)) {
        logE("getMethod(%s) exception: %s", "requestPermissions", exTypeStr);
      }
      return 1;
    }
    jint permission_granted = (*env)->GetStaticIntField(env, jPM,
        (*env)->GetStaticFieldID(env, jPM, "PERMISSION_GRANTED", "I"));

    const char* wantPerm[] = {
      "READ_EXTERNAL_STORAGE",
      "WRITE_EXTERNAL_STORAGE",
    };
    size_t missingCount = 0;
    jstring missing[sizeof(wantPerm) / sizeof(wantPerm[0])];
    for (size_t i = 0; i < sizeof(wantPerm) / sizeof(wantPerm[0]); i++) {
      missing[i] = NULL;
    }

    // Call checkSelfPermission for each wantPerm[].
    for (size_t i = 0; i < sizeof(wantPerm) / sizeof(wantPerm[0]); i++) {
      jclass jPerm = safeFindClass(env, "android/Manifest$permission");
      jfieldID field = (*env)->GetStaticFieldID(env, jPerm, wantPerm[i],
                                                "Ljava/lang/String;");
      jstring str = (jstring) (*env)->GetStaticObjectField(env, jPerm, field);
      int r = intCall(env, activity->clazz, perm, "checkSelfPermission", str);
      if (r == -42) {
        return 1;  // intCall() had a JNI error.
      }
      logE("checkSelfPermission(%s): %d\n", wantPerm[i], r);
      if (r != permission_granted) {
        missing[missingCount] = str;
        missingCount++;
      } else {
        (*env)->DeleteLocalRef(env, str);
      }
    }

    if (!missingCount) {  // If all permissions are ok.
      return 0;
    }

    // Call requestPermissions for any denied permissions.
    // Android wants to call a callback, but that requires java source code.
    jint self = 42;
    jobjectArray jMiss = (*env)->NewObjectArray(
        env, missingCount, (*env)->FindClass(env,"java/lang/String"), 0);
    for (size_t i = 0; i < missingCount; i++) {
      (*env)->SetObjectArrayElement(env, jMiss, i, missing[i]);
    }
    if (voidCall(env, activity->clazz, req, "requestPermissions", jMiss, self)) {
      logE("requestPermissions failed\n");
      (*env)->DeleteLocalRef(env, jMiss);
      return 1;
    }
    (*env)->DeleteLocalRef(env, jMiss);
  }
  return 0;
}

int RetroWeb_checkPlatform() {
  struct android_app* app = glfwGetAndroidApp();
  int mustDetach;
  JNIEnv* env = rjInit(app->activity, &mustDetach);
  int r = _androidRetroRuntimePerms(app->activity, env);
  rjDestroy(app->activity, mustDetach);
  return r;
}

#else /*__ANDROID__*/
int RetroWeb_checkPlatform() {
  return 0;  // This is a no-op.
}
#endif /*__ANDROID__*/
