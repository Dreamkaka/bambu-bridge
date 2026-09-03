#include "plugin_loader.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <stdlib.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  include <limits.h>
#  include <stdlib.h>
#endif

namespace bambu {

// ----------------------------------------------------------------------------
// PluginHandle
// ----------------------------------------------------------------------------
PluginHandle::~PluginHandle() { unload(); }

PluginHandle::PluginHandle(PluginHandle&& o) noexcept
    : handle_(o.handle_), path_(std::move(o.path_)) { o.handle_ = nullptr; }

PluginHandle& PluginHandle::operator=(PluginHandle&& o) noexcept {
    if (this != &o) {
        unload();
        handle_ = o.handle_; path_ = std::move(o.path_);
        o.handle_ = nullptr;
    }
    return *this;
}

bool PluginHandle::load(const std::string& path, std::string* err) {
    unload();
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) h = ::LoadLibraryA(path.c_str());
    if (!h) {
        if (err) *err = "LoadLibrary failed for: " + path + " (error=" +
                         std::to_string(::GetLastError()) + ")";
        return false;
    }
    handle_ = h;
#else
    void* h = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        const char* de = ::dlerror();
        if (err) *err = "dlopen failed for: " + path + " -> " +
                         (de ? de : "unknown error");
        return false;
    }
    handle_ = h;
#endif
    path_ = path;
    return true;
}

void PluginHandle::unload() {
    if (!handle_) return;
#if defined(_WIN32)
    ::FreeLibrary((HMODULE)handle_);
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
    path_.clear();
}

void* PluginHandle::resolve(const std::string& name) const {
    if (!handle_) return nullptr;
#if defined(_WIN32)
    return (void*)::GetProcAddress((HMODULE)handle_, name.c_str());
#else
    return ::dlsym(handle_, name.c_str());
#endif
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static bool path_exists(const std::string& p) {
#ifdef _WIN32
    DWORD attrs = ::GetFileAttributesA(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    return ::access(p.c_str(), F_OK) == 0;
#endif
}

std::string platform_library_name(const std::string& base, bool include_prefix) {
#if defined(_WIN32)
    return base + ".dll";
#elif defined(__APPLE__)
    return include_prefix ? ("lib" + base + ".dylib") : (base + ".dylib");
#else
    return include_prefix ? ("lib" + base + ".so") : (base + ".so");
#endif
}

static std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
#if defined(_WIN32)
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

static std::string make_absolute(const std::string& p) {
    if (p.empty()) return p;
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = ::GetFullPathNameA(p.c_str(), MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) return std::string(buf);
    return p;
#else
    char* r = ::realpath(p.c_str(), nullptr);
    if (!r) return p;
    std::string out(r);
    ::free(r);
    return out;
#endif
}

void prepare_plugin_search_path(const std::string& dir) {
    if (dir.empty()) return;
    std::string abs = make_absolute(dir);
#if defined(_WIN32)
    ::SetDllDirectoryA(abs.c_str());
#else
    const char* old = ::getenv("LD_LIBRARY_PATH");
    std::string next = abs;
    if (old && *old) {
        next.push_back(':');
        next += old;
    }
    ::setenv("LD_LIBRARY_PATH", next.c_str(), 1);
#endif
}

std::vector<PluginHandle> load_companion_plugins(const std::string& dir) {
    std::string abs_dir = make_absolute(dir);
    std::vector<PluginHandle> out;
    const char* names[] = {
#if defined(_WIN32)
        "live555", "agora_rtc_sdk", "libaosl",
        "libagora-ffmpeg", "libagora-soundtouch",
#else
        "live555", "agora_rtc_sdk", "agora-fdkaac",
#endif
    };
    for (const char* n : names) {
        PluginHandle h;
        std::string p = join_path(abs_dir, platform_library_name(n, true));
        if (!path_exists(p)) continue;
        std::string err;
        if (h.load(p, &err)) out.push_back(std::move(h));
    }
    return out;
}

PluginHandle load_network_plugin(const std::string& dir, std::string* out_error) {
    std::string abs_dir = make_absolute(dir);
    prepare_plugin_search_path(abs_dir);
    PluginHandle h;
    std::string err;
    std::vector<std::string> candidates;
    std::string pname = platform_library_name("bambu_networking", true);
    candidates.push_back(join_path(abs_dir, pname));
    candidates.push_back(join_path(abs_dir, "bambu_networking"));
    candidates.push_back(join_path(abs_dir, "libbambu_networking.so"));
    candidates.push_back(join_path(abs_dir, "bambu_networking.dll"));
    if (!abs_dir.empty() && path_exists(abs_dir) && abs_dir.find('.') != std::string::npos)
        candidates.push_back(abs_dir);

    for (const auto& c : candidates) {
        if (!path_exists(c)) continue;
        if (h.load(c, &err)) {
            if (out_error) out_error->clear();
            return h;
        }
        // collect first/last error to report on total failure
        if (out_error) *out_error = err;
    }
    if (out_error && out_error->empty())
        *out_error = "no bamboo_networking plugin found in: " + dir;
    return h;
}

PluginHandle load_source_plugin(const std::string& dir, std::string* out_error) {
    std::string abs_dir = make_absolute(dir);
    PluginHandle h;
    std::string err;
    std::string pname = platform_library_name("BambuSource", true);
    for (const auto& c : {join_path(abs_dir, pname),
                          join_path(abs_dir, "BambuSource"),}) {
        if (!path_exists(c)) continue;
        if (h.load(c, &err)) { if (out_error) out_error->clear(); return h; }
        if (out_error) *out_error = err;
    }
    if (out_error && out_error->empty())
        *out_error = "no BambuSource plugin found in: " + dir;
    return h;
}

} // namespace bambu
