

#include "license_client.h"
#include "sha256.h"

#include <jni.h>
#include <android/log.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <sys/system_properties.h>

#define TAG     "amkush/license"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

using namespace facegate_crypto;

static const uint8_t LC_BASE_URL_OBF[] = {
    0x32, 0x2e, 0x2e, 0x2a, 0x29, 0x60, 0x75, 0x75,
    0x3c, 0x3b, 0x39, 0x3f, 0x3e, 0x35, 0x39, 0x29,
    0x74, 0x38, 0x35, 0x34, 0x3e
};

static std::string lc_get_server_url() {
    std::string r;
    r.reserve(sizeof(LC_BASE_URL_OBF));
    for (size_t i = 0; i < sizeof(LC_BASE_URL_OBF); i++)
        r += (char)(LC_BASE_URL_OBF[i] ^ 0x5A);
    return r;
}

static std::string jstr(JNIEnv *env, jstring j) {
    if (!j) return {};
    const char *c = env->GetStringUTFChars(j, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(j, c);
    return s;
}

static jstring to_jstr(JNIEnv *env, const std::string &s) {
    return env->NewStringUTF(s.c_str());
}

static jobject get_system_service(JNIEnv *env, jobject ctx, const char *name) {
    jclass ctxCls  = env->GetObjectClass(ctx);
    if (!ctxCls) return nullptr;
    jmethodID mid  = env->GetMethodID(ctxCls, "getSystemService",
                                      "(Ljava/lang/String;)Ljava/lang/Object;");
    env->DeleteLocalRef(ctxCls);
    if (!mid) { env->ExceptionClear(); return nullptr; }
    jstring svcName = env->NewStringUTF(name);
    jobject svc = env->CallObjectMethod(ctx, mid, svcName);
    if (env->ExceptionCheck()) { env->ExceptionClear(); svc = nullptr; }
    env->DeleteLocalRef(svcName);
    return svc;
}

enum FieldType { FT_MISSING, FT_STRING, FT_BOOL, FT_NULL, FT_INT };

struct JField {
    std::string key;
    FieldType   type  = FT_MISSING;
    std::string sval;
    int64_t     ival  = 0;
    bool        bval  = false;
};

static size_t skip_ws(const std::string &s, size_t i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'))
        i++;
    return i;
}

static std::string parse_json_str(const std::string &s, size_t &i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    i++;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            i++;
            switch (s[i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {

                    if (i + 4 < s.size()) {
                        uint32_t cp = 0;
                        bool valid = true;
                        for (int j = 1; j <= 4; j++) {
                            char c2 = s[i + j];
                            cp <<= 4;
                            if      (c2 >= '0' && c2 <= '9') cp |= (uint32_t)(c2 - '0');
                            else if (c2 >= 'a' && c2 <= 'f') cp |= (uint32_t)(c2 - 'a' + 10);
                            else if (c2 >= 'A' && c2 <= 'F') cp |= (uint32_t)(c2 - 'A' + 10);
                            else { valid = false; break; }
                        }
                        if (valid) {
                            i += 4;

                            if (cp < 0x80) {
                                out += (char)cp;
                            } else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        } else {
                            out += 'u';
                        }
                    } else {
                        out += 'u';
                    }
                    break;
                }
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
        i++;
    }
    if (i < s.size()) i++;
    return out;
}

static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if      (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\b') { out += "\\b"; }
        else if (c == '\f') { out += "\\f"; }
        else if (c  < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
            out += buf;
        } else {
            out += (char)c;
        }
    }
    return out;
}

static std::vector<JField> json_parse_obj(const std::string &s) {
    std::vector<JField> fields;
    size_t i = skip_ws(s, 0);
    if (i >= s.size() || s[i] != '{') return fields;
    i++;

    while (true) {
        i = skip_ws(s, i);
        if (i >= s.size() || s[i] == '}') break;
        if (s[i] != '"') { i++; continue; }

        JField f;
        f.key = parse_json_str(s, i);

        i = skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') break;
        i++;
        i = skip_ws(s, i);
        if (i >= s.size()) break;

        if (s[i] == '"') {
            f.type = FT_STRING;
            f.sval = parse_json_str(s, i);
        } else if (s.compare(i, 4, "true") == 0) {
            f.type = FT_BOOL; f.bval = true; i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            f.type = FT_BOOL; f.bval = false; i += 5;
        } else if (s.compare(i, 4, "null") == 0) {
            f.type = FT_NULL; i += 4;
        } else if (s[i] == '-' || (s[i] >= '0' && s[i] <= '9')) {
            f.type = FT_INT;
            size_t start = i;
            if (s[i] == '-') i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            f.ival = strtoll(s.c_str() + start, nullptr, 10);
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}') i++;
        }

        fields.push_back(f);
        i = skip_ws(s, i);
        if (i < s.size() && s[i] == ',') i++;
    }
    return fields;
}

static std::string json_canonical(std::vector<JField> fields) {
    std::sort(fields.begin(), fields.end(),
              [](const JField &a, const JField &b){ return a.key < b.key; });

    std::string out = "{";
    bool first = true;
    for (const auto &f : fields) {
        if (f.type == FT_MISSING) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += json_escape(f.key);
        out += '"';
        out += ':';
        switch (f.type) {
            case FT_STRING:
                out += '"'; out += json_escape(f.sval); out += '"';
                break;
            case FT_BOOL:
                out += f.bval ? "true" : "false";
                break;
            case FT_NULL:
                out += "null";
                break;
            case FT_INT: {
                char buf[24];
                snprintf(buf, sizeof(buf), "%lld", (long long)f.ival);
                out += buf;
                break;
            }
            default: break;
        }
    }
    out += '}';
    return out;
}

static std::string json_get_str(const std::string &s, const char *key) {
    auto fields = json_parse_obj(s);
    for (const auto &f : fields) {
        if (f.key == key && f.type == FT_STRING) return f.sval;
    }
    return {};
}

static std::string json_get_obj_raw(const std::string &s, const char *key) {
    std::string search = "\"";
    search += key;
    search += "\"";
    size_t kpos = s.find(search);
    if (kpos == std::string::npos) return {};
    size_t i = kpos + search.size();
    i = skip_ws(s, i);
    if (i >= s.size() || s[i] != ':') return {};
    i++;
    i = skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return {};

    int depth = 0;
    size_t start = i;
    while (i < s.size()) {
        if      (s[i] == '"') { size_t tmp = i; parse_json_str(s, tmp); i = tmp; continue; }
        else if (s[i] == '{') depth++;
        else if (s[i] == '}') { depth--; if (depth == 0) { i++; break; } }
        i++;
    }
    return s.substr(start, i - start);
}

static std::string verify_envelope(const std::string &json,
                                    const std::string &sent_rid) {
    if (json.empty()) {
        LOGE("verify_envelope: empty response");
        return {};
    }

    std::string p_raw = json_get_obj_raw(json, "p");
    std::string t     = json_get_str(json, "t");
    std::string rid   = json_get_str(json, "rid");
    std::string n     = json_get_str(json, "n");
    std::string s     = json_get_str(json, "s");

    if (p_raw.empty() || t.empty() || rid.empty() || n.empty() || s.empty()) {
        LOGE("verify_envelope: missing fields");
        return {};
    }

    if (rid != sent_rid) {
        LOGE("verify_envelope: rid mismatch");
        return {};
    }

    if (t.size() < 10 || t.size() > 40) {
        LOGE("verify_envelope: suspicious timestamp length");
        return {};
    }

    auto p_fields       = json_parse_obj(p_raw);
    std::string p_canonical = json_canonical(p_fields);
    std::string ph      = sha256_hex(p_canonical);
    std::string canonical = ph + "." + t + "." + rid + "." + n;
    std::string expected  = hmac_sha256_hex(FACEGATE_HMAC_SECRET, canonical);

    if (!hex_eq(expected, s)) {
        LOGE("verify_envelope: HMAC mismatch — response tampered or wrong secret");
        return {};
    }

    LOGI("verify_envelope: OK");
    return p_canonical;
}

struct Fingerprints {
    std::string device_id;
    std::string android_id;
    std::string wifi_bssid;
    std::string wifi_ip;
    std::string build_fp;
};

static std::string get_android_id(JNIEnv *env, jobject ctx) {
    jclass secCls  = env->FindClass("android/provider/Settings$Secure");
    if (!secCls) { env->ExceptionClear(); return {}; }
    jmethodID mid  = env->GetStaticMethodID(secCls, "getString",
        "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(secCls); return {}; }

    jclass ctxCls    = env->GetObjectClass(ctx);
    if (!ctxCls) { env->DeleteLocalRef(secCls); return {}; }
    jmethodID crMid  = env->GetMethodID(ctxCls, "getContentResolver",
                                         "()Landroid/content/ContentResolver;");
    env->DeleteLocalRef(ctxCls);
    if (!crMid) { env->ExceptionClear(); env->DeleteLocalRef(secCls); return {}; }

    jobject cr = env->CallObjectMethod(ctx, crMid);
    if (env->ExceptionCheck() || !cr) {
        env->ExceptionClear(); env->DeleteLocalRef(secCls); return {};
    }

    jstring key  = env->NewStringUTF("android_id");
    jstring val  = (jstring)env->CallStaticObjectMethod(secCls, mid, cr, key);
    if (env->ExceptionCheck()) { env->ExceptionClear(); val = nullptr; }

    std::string out = jstr(env, val);

    if (val) env->DeleteLocalRef(val);
    env->DeleteLocalRef(cr);
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(secCls);
    return out;
}

static void get_wifi_info(JNIEnv *env, jobject ctx,
                           std::string &bssid_out, std::string &ip_out) {
    jobject wm = get_system_service(env, ctx, "wifi");
    if (!wm) return;

    jclass wmCls   = env->GetObjectClass(wm);
    if (!wmCls) { env->DeleteLocalRef(wm); return; }
    jmethodID gci  = env->GetMethodID(wmCls, "getConnectionInfo",
                                       "()Landroid/net/wifi/WifiInfo;");
    env->DeleteLocalRef(wmCls);
    if (!gci) { env->ExceptionClear(); env->DeleteLocalRef(wm); return; }

    jobject wi = env->CallObjectMethod(wm, gci);
    if (env->ExceptionCheck() || !wi) {
        env->ExceptionClear(); env->DeleteLocalRef(wm); return;
    }

    jclass wiCls = env->GetObjectClass(wi);
    if (!wiCls) { env->DeleteLocalRef(wi); env->DeleteLocalRef(wm); return; }

    jmethodID gBssid = env->GetMethodID(wiCls, "getBSSID", "()Ljava/lang/String;");
    if (gBssid) {
        jstring bs = (jstring)env->CallObjectMethod(wi, gBssid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); bs = nullptr; }
        std::string raw = jstr(env, bs);
        if (bs) env->DeleteLocalRef(bs);
        if (raw != "02:00:00:00:00:00" && raw != "<unknown ssid>" && !raw.empty())
            bssid_out = raw;
    } else { env->ExceptionClear(); }

    jmethodID gIp = env->GetMethodID(wiCls, "getIpAddress", "()I");
    if (gIp) {
        jint ip4 = env->CallIntMethod(wi, gIp);
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        else if (ip4 != 0) {
            char buf[20];
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                     ip4 & 0xFF, (ip4 >> 8) & 0xFF,
                     (ip4 >> 16) & 0xFF, (ip4 >> 24) & 0xFF);
            ip_out = buf;
        }
    } else { env->ExceptionClear(); }

    env->DeleteLocalRef(wiCls);
    env->DeleteLocalRef(wi);
    env->DeleteLocalRef(wm);
}

static std::string get_or_create_device_id(JNIEnv *env, jobject ctx) {
    jclass ctxCls  = env->GetObjectClass(ctx);
    if (!ctxCls) return {};
    jmethodID gsp  = env->GetMethodID(ctxCls, "getSharedPreferences",
                                       "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
    env->DeleteLocalRef(ctxCls);
    if (!gsp) { env->ExceptionClear(); return {}; }

    jstring spName = env->NewStringUTF("facegate_prefs");
    jobject prefs  = env->CallObjectMethod(ctx, gsp, spName, (jint)0);
    env->DeleteLocalRef(spName);
    if (env->ExceptionCheck() || !prefs) { env->ExceptionClear(); return {}; }

    jclass spCls   = env->GetObjectClass(prefs);
    if (!spCls) { env->DeleteLocalRef(prefs); return {}; }
    jmethodID gsId = env->GetMethodID(spCls, "getString",
                                       "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!gsId) {
        env->ExceptionClear();
        env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return {};
    }

    jstring keyJ   = env->NewStringUTF("device_id");
    jstring defJ   = env->NewStringUTF("");
    jstring valJ   = (jstring)env->CallObjectMethod(prefs, gsId, keyJ, defJ);
    if (env->ExceptionCheck()) { env->ExceptionClear(); valJ = nullptr; }
    std::string id = jstr(env, valJ);

    if (valJ) env->DeleteLocalRef(valJ);
    env->DeleteLocalRef(defJ);
    env->DeleteLocalRef(keyJ);

    if (!id.empty()) {
        env->DeleteLocalRef(spCls);
        env->DeleteLocalRef(prefs);
        return id;
    }


    jclass uuidCls   = env->FindClass("java/util/UUID");
    if (!uuidCls) {
        env->ExceptionClear();
        env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return {};
    }
    jmethodID randFn = env->GetStaticMethodID(uuidCls, "randomUUID", "()Ljava/util/UUID;");
    if (!randFn) {
        env->ExceptionClear();
        env->DeleteLocalRef(uuidCls); env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return {};
    }
    jobject uuidObj  = env->CallStaticObjectMethod(uuidCls, randFn);
    if (env->ExceptionCheck() || !uuidObj) {
        env->ExceptionClear();
        env->DeleteLocalRef(uuidCls); env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return {};
    }
    jmethodID tsId   = env->GetMethodID(uuidCls, "toString", "()Ljava/lang/String;");
    if (!tsId) {
        env->ExceptionClear();
        env->DeleteLocalRef(uuidObj); env->DeleteLocalRef(uuidCls);
        env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return {};
    }
    jstring uuidStr  = (jstring)env->CallObjectMethod(uuidObj, tsId);
    if (env->ExceptionCheck()) { env->ExceptionClear(); uuidStr = nullptr; }
    id = jstr(env, uuidStr);

    if (uuidStr) env->DeleteLocalRef(uuidStr);
    env->DeleteLocalRef(uuidObj);
    env->DeleteLocalRef(uuidCls);

    if (id.empty()) {
        env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return {};
    }


    jmethodID editFn = env->GetMethodID(spCls, "edit",
                                         "()Landroid/content/SharedPreferences$Editor;");
    if (!editFn) {
        env->ExceptionClear();
        env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return id;
    }
    jobject editor   = env->CallObjectMethod(prefs, editFn);
    if (env->ExceptionCheck() || !editor) {
        env->ExceptionClear();
        env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return id;
    }
    jclass edCls     = env->GetObjectClass(editor);
    if (!edCls) {
        env->DeleteLocalRef(editor); env->DeleteLocalRef(spCls); env->DeleteLocalRef(prefs); return id;
    }
    jmethodID putFn  = env->GetMethodID(edCls, "putString",
        "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;");
    jmethodID appFn  = env->GetMethodID(edCls, "apply", "()V");

    if (putFn && appFn) {
        jstring keyJ2  = env->NewStringUTF("device_id");
        jstring valJ2  = env->NewStringUTF(id.c_str());
        jobject result = env->CallObjectMethod(editor, putFn, keyJ2, valJ2);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (result) env->DeleteLocalRef(result);
        env->CallVoidMethod(editor, appFn);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(valJ2);
        env->DeleteLocalRef(keyJ2);
    } else {
        env->ExceptionClear();
    }

    env->DeleteLocalRef(edCls);
    env->DeleteLocalRef(editor);
    env->DeleteLocalRef(spCls);
    env->DeleteLocalRef(prefs);

    LOGI("Generated new device_id: %s", id.c_str());
    return id;
}

static std::string get_build_fingerprint(JNIEnv *env) {
    jclass cls   = env->FindClass("android/os/Build");
    if (cls) {
        jfieldID fid = env->GetStaticFieldID(cls, "FINGERPRINT", "Ljava/lang/String;");
        if (fid) {
            jstring s = (jstring)env->GetStaticObjectField(cls, fid);
            if (!env->ExceptionCheck() && s) {
                std::string out = jstr(env, s);
                env->DeleteLocalRef(s);
                env->DeleteLocalRef(cls);
                if (!out.empty()) return out;
            } else {
                env->ExceptionClear();
            }
        } else {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(cls);
    } else {
        env->ExceptionClear();
    }
    char buf[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.fingerprint", buf);
    return buf;
}

static Fingerprints gather_fingerprints(JNIEnv *env, jobject ctx) {
    Fingerprints fp;
    fp.device_id  = get_or_create_device_id(env, ctx);
    fp.android_id = get_android_id(env, ctx);
    fp.build_fp   = get_build_fingerprint(env);
    get_wifi_info(env, ctx, fp.wifi_bssid, fp.wifi_ip);
    return fp;
}

static std::string gen_rid(JNIEnv *env) {
    jclass cls   = env->FindClass("java/util/UUID");
    if (!cls) { env->ExceptionClear(); return "rid_fallback"; }
    jmethodID fn = env->GetStaticMethodID(cls, "randomUUID", "()Ljava/util/UUID;");
    if (!fn) {
        env->ExceptionClear(); env->DeleteLocalRef(cls); return "rid_fallback";
    }
    jobject o = env->CallStaticObjectMethod(cls, fn);
    if (env->ExceptionCheck() || !o) {
        env->ExceptionClear(); env->DeleteLocalRef(cls); return "rid_fallback";
    }
    jmethodID ts = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
    if (!ts) {
        env->ExceptionClear(); env->DeleteLocalRef(o); env->DeleteLocalRef(cls); return "rid_fallback";
    }
    jstring s = (jstring)env->CallObjectMethod(o, ts);
    if (env->ExceptionCheck()) { env->ExceptionClear(); s = nullptr; }
    std::string r = jstr(env, s);
    if (s) env->DeleteLocalRef(s);
    env->DeleteLocalRef(o);
    env->DeleteLocalRef(cls);
    return r.empty() ? "rid_fallback" : r;
}

static std::string http_post(JNIEnv *env, const std::string &url_str,
                              const std::string &json_body) {
    jclass urlCls  = env->FindClass("java/net/URL");
    if (!urlCls) { env->ExceptionClear(); LOGE("http_post: URL class not found"); return {}; }
    jmethodID urlInit = env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V");
    if (!urlInit) {
        env->ExceptionClear(); env->DeleteLocalRef(urlCls); return {};
    }
    jstring urlJ   = env->NewStringUTF(url_str.c_str());
    jobject urlObj = env->NewObject(urlCls, urlInit, urlJ);
    env->DeleteLocalRef(urlJ);

    if (!urlObj || env->ExceptionCheck()) {
        env->ExceptionClear(); env->DeleteLocalRef(urlCls);
        LOGE("http_post: bad URL: %s", url_str.c_str());
        return {};
    }

    jmethodID openConn = env->GetMethodID(urlCls, "openConnection",
                                           "()Ljava/net/URLConnection;");
    env->DeleteLocalRef(urlCls);
    if (!openConn) {
        env->ExceptionClear(); env->DeleteLocalRef(urlObj); return {};
    }
    jobject conn = env->CallObjectMethod(urlObj, openConn);
    env->DeleteLocalRef(urlObj);

    if (!conn || env->ExceptionCheck()) {
        env->ExceptionClear(); LOGE("http_post: openConnection failed"); return {};
    }

    jclass httpCls = env->GetObjectClass(conn);
    if (!httpCls) { env->DeleteLocalRef(conn); return {}; }


    jmethodID setMethod = env->GetMethodID(httpCls, "setRequestMethod", "(Ljava/lang/String;)V");
    if (setMethod) {
        jstring post = env->NewStringUTF("POST");
        env->CallVoidMethod(conn, setMethod, post);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(post);
    } else { env->ExceptionClear(); }


    jmethodID setProp = env->GetMethodID(httpCls, "setRequestProperty",
                                          "(Ljava/lang/String;Ljava/lang/String;)V");
    if (setProp) {
        jstring ctKey = env->NewStringUTF("Content-Type");
        jstring ctVal = env->NewStringUTF("application/json; charset=utf-8");
        env->CallVoidMethod(conn, setProp, ctKey, ctVal);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(ctKey); env->DeleteLocalRef(ctVal);
    } else { env->ExceptionClear(); }


    jmethodID setCT = env->GetMethodID(httpCls, "setConnectTimeout", "(I)V");
    jmethodID setRT = env->GetMethodID(httpCls, "setReadTimeout", "(I)V");
    if (setCT) { env->CallVoidMethod(conn, setCT, (jint)6000); if (env->ExceptionCheck()) env->ExceptionClear(); }
    else env->ExceptionClear();
    if (setRT) { env->CallVoidMethod(conn, setRT, (jint)12000); if (env->ExceptionCheck()) env->ExceptionClear(); }
    else env->ExceptionClear();


    jmethodID setDO = env->GetMethodID(httpCls, "setDoOutput", "(Z)V");
    if (setDO) { env->CallVoidMethod(conn, setDO, (jboolean)JNI_TRUE); if (env->ExceptionCheck()) env->ExceptionClear(); }
    else env->ExceptionClear();


    jclass strCls   = env->FindClass("java/lang/String");
    jmethodID gbMid = nullptr;
    if (strCls) {
        gbMid = env->GetMethodID(strCls, "getBytes", "(Ljava/lang/String;)[B");
        if (!gbMid) env->ExceptionClear();
        env->DeleteLocalRef(strCls);
    } else { env->ExceptionClear(); }

    jbyteArray bodyBytes = nullptr;
    if (gbMid) {
        jstring bodyJ = env->NewStringUTF(json_body.c_str());
        jstring utf8J = env->NewStringUTF("UTF-8");
        bodyBytes = (jbyteArray)env->CallObjectMethod(bodyJ, gbMid, utf8J);
        if (env->ExceptionCheck()) { env->ExceptionClear(); bodyBytes = nullptr; }
        env->DeleteLocalRef(utf8J);
        env->DeleteLocalRef(bodyJ);
    }

    jmethodID getOS = env->GetMethodID(httpCls, "getOutputStream", "()Ljava/io/OutputStream;");
    if (getOS && bodyBytes) {
        jobject os = env->CallObjectMethod(conn, getOS);
        if (os && !env->ExceptionCheck()) {
            jclass osCls  = env->GetObjectClass(os);
            if (osCls) {
                jmethodID write = env->GetMethodID(osCls, "write", "([B)V");
                jmethodID flush = env->GetMethodID(osCls, "flush", "()V");
                jmethodID close = env->GetMethodID(osCls, "close", "()V");
                if (write) { env->CallVoidMethod(os, write, bodyBytes); if (env->ExceptionCheck()) env->ExceptionClear(); }
                else env->ExceptionClear();
                if (flush) { env->CallVoidMethod(os, flush); if (env->ExceptionCheck()) env->ExceptionClear(); }
                else env->ExceptionClear();
                if (close) { env->CallVoidMethod(os, close); if (env->ExceptionCheck()) env->ExceptionClear(); }
                else env->ExceptionClear();
                env->DeleteLocalRef(osCls);
            }
            env->DeleteLocalRef(os);
        } else {
            env->ExceptionClear();
        }
    } else { env->ExceptionClear(); }
    if (bodyBytes) env->DeleteLocalRef(bodyBytes);


    jmethodID getCode = env->GetMethodID(httpCls, "getResponseCode", "()I");
    if (!getCode) {
        env->ExceptionClear();
        env->DeleteLocalRef(httpCls); env->DeleteLocalRef(conn); return {};
    }
    jint code = env->CallIntMethod(conn, getCode);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("http_post: getResponseCode threw (timeout?)");
        env->DeleteLocalRef(httpCls); env->DeleteLocalRef(conn); return {};
    }


    const char *streamMethod = (code < 400) ? "getInputStream" : "getErrorStream";
    jmethodID getIS = env->GetMethodID(httpCls, streamMethod, "()Ljava/io/InputStream;");
    env->DeleteLocalRef(httpCls);
    if (!getIS) {
        env->ExceptionClear(); env->DeleteLocalRef(conn); return {};
    }
    jobject is = env->CallObjectMethod(conn, getIS);
    if (env->ExceptionCheck()) { env->ExceptionClear(); is = nullptr; }

    std::string response;

    if (is) {
        jclass isrCls   = env->FindClass("java/io/InputStreamReader");
        jclass brCls    = env->FindClass("java/io/BufferedReader");

        if (isrCls && brCls) {
            jmethodID isrInit = env->GetMethodID(isrCls, "<init>",
                "(Ljava/io/InputStream;Ljava/lang/String;)V");
            jmethodID brInit  = env->GetMethodID(brCls, "<init>", "(Ljava/io/Reader;)V");
            jmethodID rl      = env->GetMethodID(brCls, "readLine", "()Ljava/lang/String;");

            if (isrInit && brInit && rl) {
                jstring utf8J2 = env->NewStringUTF("UTF-8");
                jobject isr    = env->NewObject(isrCls, isrInit, is, utf8J2);
                env->DeleteLocalRef(utf8J2);
                if (isr && !env->ExceptionCheck()) {
                    jobject br = env->NewObject(brCls, brInit, isr);
                    env->DeleteLocalRef(isr);
                    if (br && !env->ExceptionCheck()) {
                        while (true) {
                            jstring line = (jstring)env->CallObjectMethod(br, rl);
                            if (!line || env->ExceptionCheck()) {
                                env->ExceptionClear();
                                if (line) env->DeleteLocalRef(line);
                                break;
                            }
                            response += jstr(env, line);
                            env->DeleteLocalRef(line);
                        }

                        jclass brCls2   = env->GetObjectClass(br);
                        if (brCls2) {
                            jmethodID close = env->GetMethodID(brCls2, "close", "()V");
                            if (close) { env->CallVoidMethod(br, close); if (env->ExceptionCheck()) env->ExceptionClear(); }
                            else env->ExceptionClear();
                            env->DeleteLocalRef(brCls2);
                        }
                        env->DeleteLocalRef(br);
                    } else {
                        env->ExceptionClear();
                    }
                } else {
                    env->ExceptionClear();
                    if (isr) env->DeleteLocalRef(isr);
                }
            } else {
                env->ExceptionClear();
            }
        } else {
            env->ExceptionClear();
        }
        if (isrCls) env->DeleteLocalRef(isrCls);
        if (brCls)  env->DeleteLocalRef(brCls);
        env->DeleteLocalRef(is);
    }

    env->DeleteLocalRef(conn);
    LOGI("http_post: %s  code=%d  resp_len=%zu", url_str.c_str(), (int)code, response.size());
    return response;
}

static std::string build_activate_body(const Fingerprints &fp,
                                        const std::string &key,
                                        const std::string &rid) {
    auto esc = [](const std::string &s) { return json_escape(s); };
    std::string b = "{";
    b += "\"key\":\""        + esc(key)           + "\",";
    b += "\"device_id\":\""  + esc(fp.device_id)  + "\",";
    b += "\"android_id\":\"" + esc(fp.android_id) + "\",";
    b += "\"wifi_bssid\":\"" + esc(fp.wifi_bssid) + "\",";
    b += "\"wifi_ip\":\""    + esc(fp.wifi_ip)     + "\",";
    b += "\"build_fp\":\""   + esc(fp.build_fp)   + "\",";
    b += "\"rid\":\""        + esc(rid)            + "\"";
    b += "}";
    return b;
}

static std::string build_verify_body(const Fingerprints &fp,
                                      const std::string &token,
                                      const std::string &rid) {
    auto esc = [](const std::string &s) { return json_escape(s); };
    std::string b = "{";
    b += "\"device_id\":\""  + esc(fp.device_id)  + "\",";
    b += "\"android_id\":\"" + esc(fp.android_id) + "\",";
    if (!token.empty())
        b += "\"token\":\""  + esc(token)          + "\",";
    b += "\"rid\":\""        + esc(rid)            + "\"";
    b += "}";
    return b;
}

static std::string build_heartbeat_body(const Fingerprints &fp,
                                         const std::string &rid) {
    auto esc = [](const std::string &s) { return json_escape(s); };
    std::string b = "{";
    b += "\"device_id\":\""  + esc(fp.device_id)  + "\",";
    b += "\"android_id\":\"" + esc(fp.android_id) + "\",";
    b += "\"rid\":\""        + esc(rid)            + "\"";
    b += "}";
    return b;
}

static std::string do_call(JNIEnv *env, const std::string &endpoint,
                            const std::string &body, const std::string &rid) {
    std::string url  = lc_get_server_url() + endpoint;
    std::string resp = http_post(env, url, body);
    if (resp.empty()) {
        LOGE("do_call: empty response from %s", endpoint.c_str());
        return {};
    }
    return verify_envelope(resp, rid);
}

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_itsme_amkush_license_LicenseClient_nativeActivate(
        JNIEnv *env, jclass , jobject context, jstring keyJ)
{
    std::string key = jstr(env, keyJ);
    if (key.empty()) {
        LOGE("nativeActivate: null key");
        return env->NewStringUTF("");
    }

    Fingerprints fp = gather_fingerprints(env, context);
    if (fp.device_id.empty()) {
        LOGE("nativeActivate: could not obtain device_id");
        return env->NewStringUTF("");
    }


    LOGI("nativeActivate: key_len=%zu dev_prefix=%.4s...", key.size(),
         fp.device_id.size() >= 4 ? fp.device_id.c_str() : "???");

    std::string rid     = gen_rid(env);
    std::string body    = build_activate_body(fp, key, rid);
    std::string payload = do_call(env, "/api/activate", body, rid);
    return env->NewStringUTF(payload.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_itsme_amkush_license_LicenseClient_nativeVerify(
        JNIEnv *env, jclass , jobject context, jstring tokenJ)
{
    std::string token = jstr(env, tokenJ);

    Fingerprints fp = gather_fingerprints(env, context);
    if (fp.device_id.empty()) {
        LOGE("nativeVerify: could not obtain device_id");
        return env->NewStringUTF("");
    }

    std::string rid     = gen_rid(env);
    std::string body    = build_verify_body(fp, token, rid);
    std::string payload = do_call(env, "/api/verify", body, rid);
    return env->NewStringUTF(payload.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_itsme_amkush_license_LicenseClient_nativeHeartbeat(
        JNIEnv *env, jclass , jobject context)
{
    Fingerprints fp = gather_fingerprints(env, context);
    if (fp.device_id.empty()) {
        LOGE("nativeHeartbeat: could not obtain device_id");
        return env->NewStringUTF("");
    }

    std::string rid     = gen_rid(env);
    std::string body    = build_heartbeat_body(fp, rid);
    std::string payload = do_call(env, "/api/heartbeat", body, rid);
    return env->NewStringUTF(payload.c_str());
}

}

extern "C"
int license_verify_local_token(const char *token,
                                const char *device_id,
                                const char *key_text,
                                int         key_id)
{
    if (!token || !device_id || !key_text) {
        LOGE("license_verify_local_token: null argument");
        return 0;
    }

    char key_id_str[16];
    snprintf(key_id_str, sizeof(key_id_str), "%d", key_id);

    std::string raw = "facegate:paid:";
    raw += device_id;
    raw += ":";
    raw += key_id_str;
    raw += ":";
    raw += key_text;

    std::string expected = hmac_sha256_hex(FACEGATE_HMAC_SECRET, raw);
    std::string received = token;

    int ok = hex_eq(expected, received) ? 1 : 0;
    LOGI("license_verify_local_token: %s", ok ? "VALID" : "INVALID");
    return ok;
}
