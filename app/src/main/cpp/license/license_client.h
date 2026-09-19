

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <jni.h>

#ifndef FACEGATE_HMAC_SECRET
#define FACEGATE_HMAC_SECRET \
    "a7f3c9e1b8d4025f6a4b9c0e7d1f8a3b5c2e6d9f0a1b4c7d8e2f5a9b3c6d0e7f"
#endif

#define FACEGATE_TRIAL_KEY  "NOWORNEVER"

#ifdef __cplusplus
#include <string>

struct LicenseResult {
    bool        envelope_ok;
    bool        ok;
    char        access[8];
    char        token[72];
    char        reason[32];
    bool        destruct;
    int32_t     remaining_seconds;
    char        expires_at[36];
};

inline LicenseResult lr_deny(const char *reason_code = "unknown") {
    LicenseResult r{};
    r.envelope_ok        = false;
    r.ok                 = false;
    r.destruct           = true;
    r.remaining_seconds  = -1;
    r.access[0]          = '\0';
    r.token[0]           = '\0';
    r.expires_at[0]      = '\0';
    size_t n = 0;
    while (reason_code[n] && n < sizeof(r.reason) - 1) {
        r.reason[n] = reason_code[n]; n++;
    }
    r.reason[n] = '\0';
    return r;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

int license_verify_local_token(const char *token,
                                const char *device_id,
                                const char *key_text,
                                int         key_id);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jstring JNICALL
Java_com_itsme_amkush_license_LicenseClient_nativeActivate(
        JNIEnv *env, jclass ,
        jobject context, jstring key);

JNIEXPORT jstring JNICALL
Java_com_itsme_amkush_license_LicenseClient_nativeVerify(
        JNIEnv *env, jclass ,
        jobject context, jstring token);

JNIEXPORT jstring JNICALL
Java_com_itsme_amkush_license_LicenseClient_nativeHeartbeat(
        JNIEnv *env, jclass ,
        jobject context);

#ifdef __cplusplus
}
#endif
