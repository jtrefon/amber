#ifndef AGENT_TESTS_TASK_STATE_PRINT_H
#define AGENT_TESTS_TASK_STATE_PRINT_H

// The failure message prints a task record's enums by name. One definition for
// every suite that asserts on them; a per-file copy trips the duplicate gate.

#include "agent/task.h"

#include <ostream>

namespace agent {

inline std::ostream& operator<<(std::ostream& os, TaskState state) {
    return os << task_state_name(state);
}

inline std::ostream& operator<<(std::ostream& os, TaskKind kind) {
    return os << task_kind_name(kind);
}

inline std::ostream& operator<<(std::ostream& os, TaskNotify notify) {
    switch (notify) {
    case TaskNotify::Auto:
        return os << "auto";
    case TaskNotify::Digest:
        return os << "digest";
    case TaskNotify::Silent:
        return os << "silent";
    }
    return os << "auto";
}

} // namespace agent

#endif // AGENT_TESTS_TASK_STATE_PRINT_H
