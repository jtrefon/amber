
#include "agent/config.h"
#include "agent/providers.h"

// Adapter: user-added providers persisted as key=value files under
// ~/.config/amber/providers/<name>.conf.

#include <filesystem>
#include <fstream>
#include <system_error>

namespace agent {

namespace {

namespace fs = std::filesystem;

std::string providers_dir() {
    std::string dir = global_config_dir() + "/providers";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::optional<Provider> parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    Provider p;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        if (key == "provider") p.name = val;
        else if (key == "api_base") p.api_base = val;
        else if (key == "api_key") p.api_key = val;
        else if (key == "default_model" || key == "model")
            p.default_model = val;
        else if (key == "requires_key") p.requires_key = (val == "1" || val == "true");
        else if (key == "default_context_size")
            p.default_context_size = std::atoi(val.c_str());
    }
    return p;
}

class FileProviderRepository : public ProviderRepository {
public:
    std::vector<Provider> all() const override {
        std::vector<Provider> out;
        const std::string dir = providers_dir();
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            std::string name = entry.path().filename().string();
            if (name.size() < 5 || name.substr(name.size() - 5) != ".conf")
                continue;
            if (auto p = parse_file(entry.path().string()))
                out.push_back(std::move(*p));
        }
        return out;
    }

    std::optional<Provider> find(const std::string& name) const override {
        return parse_file(providers_dir() + "/" + name + ".conf");
    }

    bool save(const Provider& p) override {
        const std::string path = providers_dir() + "/" + p.name + ".conf";
        std::ofstream f(path, std::ios::trunc);
        if (!f) return false;
        f << "# amber provider: " << p.name << "\n";
        f << "provider=" << p.name << "\n";
        f << "api_base=" << p.api_base << "\n";
        f << "api_key=" << p.api_key << "\n";
        f << "default_model=" << p.default_model << "\n";
        f << "requires_key=" << (p.requires_key ? "1" : "0") << "\n";
        if (p.default_context_size > 0)
            f << "default_context_size=" << p.default_context_size << "\n";
        return static_cast<bool>(f);
    }

    bool remove(const std::string& name) override {
        return fs::remove(providers_dir() + "/" + name + ".conf");
    }
};

} // namespace

std::unique_ptr<ProviderRepository> make_file_provider_repository() {
    return std::make_unique<FileProviderRepository>();
}

} // namespace agent
