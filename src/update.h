#ifndef AMBER_SRC_UPDATE_H
#define AMBER_SRC_UPDATE_H

#include <string>
#include <vector>

// `amber-cli update`: hand the upgrade to the native package manager instead of
// growing an updater of our own.

enum class PackageManager { Apt, Dnf, Brew, Unknown };

// The upgrade as data: the command a user is shown, and the steps that are run
// when they confirm. A manager that needs its metadata refreshed first is two
// steps, expressed as two programs rather than one shell string.
struct UpdatePlan {
    PackageManager manager = PackageManager::Unknown;
    std::string command;
    std::vector<std::vector<std::string>> steps;
};

UpdatePlan update_plan_for(PackageManager manager);

// Which package manager this system has. Pure probing, no side effects.
PackageManager detect_package_manager() noexcept;

// Returns the exit code, or -1 when this invocation did not ask for an update.
int run_update_surface(int argc, char** argv);

#endif // AMBER_SRC_UPDATE_H
