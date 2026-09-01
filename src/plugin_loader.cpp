#include "plugin_loader.hpp"

#include <cstdlib>
#include <cstring>
#include <unistd.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
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
    HMODULE h = ::LoadLibraryA(path.c_str());
    if (!h) {
        if (err) *err = "LoadLibrary failed for: " + path + " (error=" +
                         std::to_string(::GetLastError()) + ")";
        return false;
    }
    handle_ = h;
#else
    void* h = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        if (err) *err = "dlopen failed for: " + path + " -> " +
                         (::dlerror() ? ::dlerror() : "unknown error");
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
    return dir + "/" + name;
}

PluginHandle load_network_plugin(const std::string& dir, std::string* out_error) {
    PluginHandle h;
    std::string err;
    std::vector<std::string> candidates;
    std::string pname = platform_library_name("bambu_networking", true);
    candidates.push_back(join_path(dir, pname));                       // libbambu_networking.so / bamboo_networking.dll
    candidates.push_back(join_path(dir, "bambu_networking"));          // literal base
    candidates.push_back(join_path(dir, "libbambu_networking.so"));    // explicit linux name
    candidates.push_back(join_path(dir, "bambu_networking.dll"));      // explicit windows name
    if (!dir.empty()) candidates.push_back(dir);                        // treat dir itself as a file

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
    PluginHandle h;
    std::string err;
    std::string pname = platform_library_name("BambuSource", true);
    for (const auto& c : {join_path(dir, pname),
                          join_path(dir, "BambuSource"),}) {
        if (!path_exists(c)) continue;
        if (h.load(c, &err)) { if (out_error) out_error->clear(); return h; }
        if (out_error) *out_error = err;
    }
    if (out_error && out_error->empty())
        *out_error = "no BambuSource plugin found in: " + dir;
    return h;
}

} // namespace bambu
