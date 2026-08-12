#include <jni.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>

static std::string json_quote(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default: {
                const auto byte = static_cast<unsigned char>(ch);
                if (byte < 0x20) {
                    out << "\\u00";
                    const char *hex = "0123456789abcdef";
                    out << hex[(byte >> 4) & 0x0f] << hex[byte & 0x0f];
                } else {
                    out << ch;
                }
            }
        }
    }
    out << '"';
    return out.str();
}

static jstring to_jstring(JNIEnv *env, const std::string &value) {
    return env->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_statPath(JNIEnv *env, jobject, jstring path) {
    const char *native_path = path == nullptr ? nullptr : env->GetStringUTFChars(path, nullptr);
    struct stat st = {};
    bool ok = false;
    int saved_errno = 0;
    if (native_path != nullptr && native_path[0] != '\0') {
        if (stat(native_path, &st) == 0) {
            ok = true;
        } else {
            saved_errno = errno;
        }
    } else {
        saved_errno = EINVAL;
    }
    if (native_path != nullptr) {
        env->ReleaseStringUTFChars(path, native_path);
    }
    const unsigned long ino = ok ? static_cast<unsigned long>(st.st_ino) : 0UL;
    const unsigned long dev = ok ? static_cast<unsigned long>(st.st_dev) : 0UL;
    const unsigned long mode = ok ? static_cast<unsigned long>(st.st_mode) : 0UL;
    const unsigned int major_id = ok ? major(st.st_dev) : 0u;
    const unsigned int minor_id = ok ? minor(st.st_dev) : 0u;
    std::ostringstream out;
    out << "{"
        << "\"ok\":" << (ok ? "true" : "false") << ","
        << "\"errno\":" << saved_errno << ","
        << "\"error\":" << json_quote(ok ? "" : std::strerror(saved_errno)) << ","
        << "\"ino\":" << ino << ","
        << "\"dev\":" << dev << ","
        << "\"dev_major\":" << major_id << ","
        << "\"dev_minor\":" << minor_id << ","
        << "\"mode\":" << mode
        << "}";
    return to_jstring(env, out.str());
}
