#ifndef AGENT_PLUGINS_PROJECT_INDEX_STORE_H
#define AGENT_PLUGINS_PROJECT_INDEX_STORE_H

#include <string>
#include <vector>

namespace agent::plugins::index_store {

// The index file's operations, in one place. The reader (the plugin) and the
// writers (the tools) must agree on one shape, and the rule about what may be
// written belongs where the write happens -- not in a written request to the
// model to be careful.

// The file as it stands, or "" when the project has none yet.
std::string read_all();

// Replace the whole file. Refuses text that looks like a credential, filling
// `error` with why; on success appends one line to the log.
bool write_all(const std::string& body, std::string& error);

// One entry under a section heading, creating the file and the heading when
// they are missing. `section` is a key ("shape", "runs", "where", "selection")
// or a heading title to use as given.
bool add_entry(const std::string& section, const std::string& entry, std::string& error);

// Replace the first line containing `find` with `entry`; remove the first line
// containing `match`. Both report a missing match rather than doing nothing
// quietly, so a typo is visible.
bool update_line(const std::string& find, const std::string& entry, std::string& error);
bool remove_line(const std::string& match, std::string& error);

// The heading a section key names. An unknown key is a heading title.
std::string section_title(const std::string& key);

// Credential-shaped text. The index is carried in every prompt and read by
// anyone with the state dir, so a value never goes in -- a pointer does.
bool looks_like_secret(const std::string& text);

// The append log, next to the index: every mutation, so two windows cannot
// silently lose each other's entries and a change can be traced back.
std::string log_path();

} // namespace agent::plugins::index_store

#endif // AGENT_PLUGINS_PROJECT_INDEX_STORE_H
