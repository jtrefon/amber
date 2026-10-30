#include "update.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool executable(const char* path) noexcept {
    return access(path, X_OK) == 0;
}

int run_step(const std::vector<std::string>& words) {
    std::vector<char*> argv;
    argv.reserve(words.size() + 1);
    for (const auto& word : words)
        argv.push_back(const_cast<char*>(word.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "amber update: cannot start " << words.front() << ": "
                  << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv.data());
        std::_Exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            std::cerr << "amber update: lost " << words.front() << ": "
                      << std::strerror(errno) << "\n";
            return 1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

bool confirmed() {
    std::cout << "Proceed? [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
        return false;
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

void print_missing_manager() {
    std::cerr << "amber update: no supported package manager found.\n"
              << "  Debian, Ubuntu: sudo apt-get install --only-upgrade amber-agent\n"
              << "  Fedora:         sudo dnf upgrade amber-agent\n"
              << "  macOS:          brew upgrade amber\n";
}

// Nothing is installed without an explicit confirmation on a terminal. A
// non-interactive caller gets the command and a non-zero status, because the
// one thing it must not conclude is "the update ran".
int perform(const UpdatePlan& plan) {
    if (plan.manager == PackageManager::Brew) {
        std::cout << "Homebrew owns this prefix; run the command above.\n";
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "amber update: not run: no terminal. Re-run in a shell, or use --print.\n";
        return 2;
    }
    if (!confirmed())
        return 1;
    for (const auto& step : plan.steps) {
        const int rc = run_step(step);
        if (rc != 0)
            return rc;
    }
    std::cout << "amber update: done. Restart amber to run the new build.\n";
    return 0;
}

void print_usage() {
    std::cout << "Usage: amber-cli update [--print]\n"
              << "Upgrade the installed amber-agent package with the system package\n"
              << "manager, or with --print, show the command without running it.\n";
}

} // namespace

UpdatePlan update_plan_for(PackageManager manager) {
    UpdatePlan plan;
    plan.manager = manager;
    switch (manager) {
    case PackageManager::Apt:
        plan.command = "sudo apt-get update && sudo apt-get install --only-upgrade amber-agent";
        plan.steps = {{"sudo", "apt-get", "update"},
                      {"sudo", "apt-get", "install", "--only-upgrade", "amber-agent"}};
        break;
    case PackageManager::Dnf:
        plan.command = "sudo dnf upgrade amber-agent";
        plan.steps = {{"sudo", "dnf", "upgrade", "amber-agent"}};
        break;
    case PackageManager::Brew:
        // amber never writes into the Cellar; the command is the whole offer.
        plan.command = "brew upgrade amber";
        break;
    case PackageManager::Unknown:
        break;
    }
    return plan;
}

PackageManager detect_package_manager() noexcept {
#ifdef __APPLE__
    if (executable("/opt/homebrew/bin/brew") || executable("/usr/local/bin/brew"))
        return PackageManager::Brew;
    return PackageManager::Unknown;
#else
    if (executable("/usr/bin/apt-get"))
        return PackageManager::Apt;
    if (executable("/usr/bin/dnf") || executable("/usr/sbin/dnf") || executable("/usr/bin/dnf5") ||
        executable("/usr/sbin/dnf5"))
        return PackageManager::Dnf;
    return PackageManager::Unknown;
#endif
}

int run_update_surface(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "update")
        return -1;
    bool print_only = false;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--print") {
            print_only = true;
        } else if (flag == "-h" || flag == "--help") {
            print_usage();
            return 0;
        } else {
            std::cerr << "amber update: unknown option " << flag << "\n";
            return 2;
        }
    }

    const UpdatePlan plan = update_plan_for(detect_package_manager());
    if (plan.manager == PackageManager::Unknown) {
        print_missing_manager();
        return 1;
    }
    std::cout << "amber update: " << plan.command << "\n";
    if (print_only)
        return 0;
    return perform(plan);
}
