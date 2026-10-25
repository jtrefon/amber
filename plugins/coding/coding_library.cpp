#include "coding_library.h"

#include "agent/data_path.h"
#include "plugins/project_index/index_store.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;

// The working index rides in every turn, so its size is a real cost: one line
// per selected entry, and no more than this many of them. Ranking is what makes
// the cap tolerable -- the heaviest entries are the ones that survive it.
constexpr std::size_t kMaxEntries = 25;

// The default weight, and the ceiling a header may claim. A topic outranks an
// entry by default because pulling it reaches many entries at once.
constexpr int kDefaultWeight = 50;
constexpr int kTopicWeight = 60;

constexpr const char* kLibraryRel = "prompts/domains/coding/library";
constexpr const char* kTopicManifest = "topic.md";
constexpr const char* kVocabulary = "tags.md";

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string to_lower(std::string text);

std::vector<std::string> split_tags(const std::string& value) {
    std::vector<std::string> tags;
    std::istringstream in(value);
    std::string item;
    while (in >> item) {
        std::string tag = trim(item);
        while (!tag.empty() && (tag.back() == ',' || tag.back() == ';'))
            tag.pop_back();
        if (!tag.empty() && tag[0] == '#')
            tag.erase(0, 1);
        if (!tag.empty())
            tags.push_back(tag);
    }
    return tags;
}

// Aliases are phrases, not tags: "event sourcing" and "OOP" have to survive
// whole. Split on the two separators the vocabulary uses, trim, lowercase.
std::vector<std::string> split_aliases(const std::string& value) {
    std::vector<std::string> aliases;
    std::istringstream commas(value);
    std::string item;
    while (std::getline(commas, item, ',')) {
        std::istringstream semis(item);
        std::string part;
        while (std::getline(semis, part, ';')) {
            std::string alias = to_lower(trim(part));
            if (!alias.empty())
                aliases.push_back(std::move(alias));
        }
    }
    return aliases;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The index needs the header only. Reading every entry's whole body once per
// turn is I/O the block does not use -- the body is fetched through the tool
// when the entry is actually pulled.
std::string read_header_text(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty())
            break;
        out += line;
        out += '\n';
    }
    return out;
}

bool is_markdown(const std::string& name) {
    return name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0 && name[0] != '.';
}

std::string stem_of(const std::string& name) {
    return is_markdown(name) ? name.substr(0, name.size() - 3) : name;
}

std::string to_lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A weight a header may carry: plain digits, clamped. Anything else is the
// default rather than an error -- a malformed weight should not hide knowledge.
int parse_weight(const std::string& value) {
    if (value.empty() || value.size() > 3)
        return kDefaultWeight;
    int weight = 0;
    for (char c : value) {
        if (c < '0' || c > '9')
            return kDefaultWeight;
        weight = (weight * 10) + (c - '0');
    }
    return std::min(weight, 100);
}

// How many entries a topic holds: its own files, minus the manifest and the
// vocabulary, if they live inside it.
std::size_t count_children(const fs::path& dir) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        if (!item.is_regular_file(ec))
            continue;
        const std::string name = item.path().filename().string();
        if (is_markdown(name) && name != kTopicManifest && name != kVocabulary)
            ++count;
    }
    return count;
}

// The entry's name is where it lives: the file's stem, or a topic's directory
// name. Stable, unique, and the same string the working index renders -- a
// header's prose name could be none of those.
LibraryEntry read_entry(const std::string& path, const std::string& name) {
    LibraryEntry entry = parse_library_entry(path, read_header_text(path));
    entry.name = name;
    return entry;
}

void collect_entries(const std::string& dir, std::vector<LibraryEntry>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return;
    for (const auto& item : it) {
        const std::string name = item.path().filename().string();
        if (name.empty() || name[0] == '.' || name == kVocabulary)
            continue; // the vocabulary is a document, not an entry
        if (item.is_regular_file(ec) && is_markdown(name)) {
            out.push_back(read_entry(item.path().string(), stem_of(name)));
            continue;
        }
        if (!item.is_directory(ec))
            continue;
        // A topic is a directory whose manifest declares its line. Without one
        // it is not selectable: a directory nobody wrote a trigger for would
        // otherwise appear as a blank line.
        const fs::path manifest = item.path() / kTopicManifest;
        if (!fs::is_regular_file(manifest, ec))
            continue;
        LibraryEntry topic = read_entry(manifest.string(), name);
        topic.name = name;
        topic.topic = true;
        topic.children = count_children(item.path());
        if (topic.weight == kDefaultWeight)
            topic.weight = kTopicWeight;
        out.push_back(std::move(topic));
    }
}

bool heavier_first(const LibraryEntry& a, const LibraryEntry& b) {
    if (a.weight != b.weight)
        return a.weight > b.weight;
    return a.name < b.name;
}

void append_line(std::string& out, const LibraryEntry& entry) {
    // Piecewise: a concatenation that builds a temporary per entry is the shape
    // the linter is right about, and this line is a per-turn cost.
    out += "- ";
    out += entry.trigger;
    if (entry.topic && entry.children > 0) {
        out += " (";
        out += std::to_string(entry.children);
        out += " entries)";
    }
    // The body is fetched through the tool rather than by path: the library
    // lives with the install, not with the project's workspace, so a path the
    // workspace-confined read tool cannot open would be worse than no path.
    out += " — library_lookup(\"";
    out += entry.name;
    out += "\")\n";
}

// A word in a field, or (for a word of four characters or more) the start of
// one: "sanitizer" reaches "sanitizers", "iterator" reaches "iterator", and
// "oop" no longer reaches "loop".
bool word_hit(const std::string& token, const std::set<std::string>& words) {
    if (words.count(token) > 0)
        return true;
    if (token.size() < 4)
        return false;
    return std::any_of(words.begin(), words.end(),
                       [&token](const std::string& word) { return word.rfind(token, 0) == 0; });
}

std::vector<std::string> all_words(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            current.push_back(static_cast<char>(std::tolower(c)));
            continue;
        }
        if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty())
        words.push_back(std::move(current));
    return words;
}

std::set<std::string> word_set(const std::string& text) {
    const auto words = all_words(text);
    return {words.begin(), words.end()};
}

// The words a query is made of, without the filler that would match every
// entry. A question is allowed: "why does my iterator get invalidated" keeps
// iterator and invalidated and drops the rest.
std::vector<std::string> query_words(const std::string& text) {
    static const std::set<std::string> kStop = {
        "a",    "about", "after", "all",   "also",   "am",    "an",    "and",   "any",   "are",
        "as",   "at",    "be",    "been",  "before", "being", "but",   "by",    "can",   "could",
        "did",  "do",    "does",  "for",   "from",   "get",   "has",   "have",  "here",  "how",
        "i",    "if",    "in",    "into",  "is",     "it",    "its",   "me",    "my",    "not",
        "of",   "on",    "or",    "our",   "should", "so",    "some",  "that",  "the",   "their",
        "them", "then",  "there", "these", "they",   "this",  "those", "to",    "under", "use",
        "used", "using", "was",   "we",    "were",   "what",  "when",  "where", "which", "who",
        "why",  "will",  "with",  "would", "you",    "your"};
    std::vector<std::string> out;
    for (auto& word : all_words(text))
        if (word.size() > 1 && kStop.count(word) == 0)
            out.push_back(std::move(word));
    return out;
}

std::string normalize_phrase(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
        }
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool phrase_in(const std::string& haystack, const std::string& phrase) {
    if (phrase.empty())
        return false;
    return (" " + haystack + " ").find(" " + phrase + " ") != std::string::npos;
}

std::set<std::string> lower_tags(const LibraryEntry& entry) {
    std::set<std::string> tags;
    for (const auto& tag : entry.tags)
        tags.insert(to_lower(tag[0] == '#' ? tag.substr(1) : tag));
    return tags;
}

// A declared synonym of a tag this entry carries: "OOP" reaches every design
// entry without each entry repeating the word.
int tag_alias_hit(const LibraryEntry& entry, const std::string& normalized_query,
                  const std::map<std::string, std::vector<std::string>>& aliases) {
    const auto tags = lower_tags(entry);
    for (const auto& [tag, synonyms] : aliases) {
        if (tags.count(tag) == 0)
            continue;
        for (const auto& synonym : synonyms)
            if (phrase_in(normalized_query, normalize_phrase(synonym)))
                return 3;
    }
    return 0;
}

} // namespace

LibraryEntry parse_library_entry(const std::string& path, const std::string& text) {
    LibraryEntry entry;
    entry.path = path;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty())
            break; // the body starts here; the header is the leading block
        const std::size_t colon = trimmed.find(':');
        if (colon == std::string::npos)
            break; // not a header line: the header is over
        const std::string key = to_lower(trim(trimmed.substr(0, colon)));
        const std::string value = trim(trimmed.substr(colon + 1));
        if (key == "name")
            entry.name = value;
        else if (key == "trigger")
            entry.trigger = value;
        else if (key == "tags")
            entry.tags = split_tags(value);
        else if (key == "also")
            entry.aliases = split_aliases(value);
        else if (key == "weight")
            entry.weight = parse_weight(value);
    }
    return entry;
}

std::vector<LibraryEntry> library_entries() {
    std::vector<LibraryEntry> entries;
    const std::string dir = library_dir();
    if (!dir.empty())
        collect_entries(dir, entries);
    std::sort(entries.begin(), entries.end(), heavier_first);
    return entries;
}

bool entry_matches(const LibraryEntry& entry, const std::vector<std::string>& wanted) {
    return std::any_of(entry.tags.begin(), entry.tags.end(), [&wanted](const std::string& tag) {
        const std::string bare = tag[0] == '#' ? tag.substr(1) : tag;
        return std::find(wanted.begin(), wanted.end(), bare) != wanted.end();
    });
}

std::vector<std::string> selected_tags_from(const std::string& index_body) {
    std::vector<std::string> tags;
    const std::size_t at = index_body.find("## Library selection");
    if (at == std::string::npos)
        return tags;
    const std::size_t end = index_body.find("\n## ", at + 1);
    std::istringstream lines(
        index_body.substr(at, end == std::string::npos ? std::string::npos : end - at));
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (first) {
            first = false; // the heading itself
            continue;
        }
        for (auto& tag : split_tags(line))
            tags.push_back(std::move(tag));
    }
    return tags;
}

std::map<std::string, std::vector<std::string>> tag_aliases() {
    std::map<std::string, std::vector<std::string>> aliases;
    const std::string dir = library_dir();
    if (dir.empty())
        return aliases;
    std::ifstream in(fs::path(dir) / kVocabulary);
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.rfind("- `", 0) != 0)
            continue;
        const std::size_t open = trimmed.find('`');
        const std::size_t close = trimmed.find('`', open + 1);
        if (close == std::string::npos)
            continue;
        const std::string tag = to_lower(trimmed.substr(open + 1, close - open - 1));
        const std::size_t also = to_lower(trimmed).find("also:");
        if (also == std::string::npos)
            continue;
        aliases[tag] = split_aliases(trimmed.substr(also + std::string("also:").size()));
    }
    return aliases;
}

const std::vector<std::string>& craft_tags() {
    static const std::vector<std::string> kCraft = {"design", "patterns", "architecture"};
    return kCraft;
}

namespace {

struct TokenScore {
    int total = 0;
    int strong = 0;
    int body_hits = 0;
};

int field_weight(const std::string& token, const std::set<std::string>& name,
                 const std::set<std::string>& trigger, const std::set<std::string>& body,
                 const std::set<std::string>& tags) {
    if (word_hit(token, name))
        return 3;
    if (word_hit(token, trigger))
        return 2;
    if (tags.count(token) > 0)
        return 2;
    if (word_hit(token, body))
        return 1;
    return 0;
}

TokenScore score_tokens(const std::vector<std::string>& tokens, const LibraryEntry& entry) {
    const auto name = word_set(entry.name);
    const auto trigger = word_set(entry.trigger);
    const auto body = word_set(read_text(entry.path));
    const auto tags = lower_tags(entry);
    TokenScore score;
    for (const auto& token : tokens) {
        const int weight = field_weight(token, name, trigger, body, tags);
        score.total += weight;
        if (weight >= 2)
            ++score.strong;
        if (weight == 1)
            ++score.body_hits;
    }
    return score;
}

int alias_score(const LibraryEntry& entry, const std::string& normalized,
                const std::map<std::string, std::vector<std::string>>& aliases) {
    for (const auto& phrase : entry.aliases)
        if (phrase_in(normalized, normalize_phrase(phrase)))
            return 3;
    return tag_alias_hit(entry, normalized, aliases);
}

} // namespace

int entry_query_score(const LibraryEntry& entry, const std::string& query,
                      const std::map<std::string, std::vector<std::string>>& aliases) {
    const auto tokens = query_words(query);
    if (tokens.empty())
        return 0;
    const TokenScore score = score_tokens(tokens, entry);
    const int alias = alias_score(entry, normalize_phrase(query), aliases);

    // A single body mention is noise ("loop" in a build entry); a name, a
    // trigger, a tag or a declared synonym is evidence.
    if (score.strong == 0 && alias == 0 && score.body_hits < 2)
        return 0;
    const int total = score.total + alias;
    return (total * 100) / static_cast<int>(3 * tokens.size()) + entry.weight / 10;
}

bool entry_matches_query(const LibraryEntry& entry, const std::string& query,
                         const std::map<std::string, std::vector<std::string>>& aliases) {
    return entry_query_score(entry, query, aliases) > 0;
}

std::string library_dir() {
    // A directory, resolved with the same candidate order as the prompts: a
    // packaged install keeps it under <prefix>/share/amber, away from the
    // binary, and a same-named file must not be mistaken for the tree.
    return resolve_data_dir(kLibraryRel, exe_path().c_str());
}

std::vector<LibraryEntry> topic_entries(const std::string& dir) {
    std::vector<LibraryEntry> entries;
    if (!dir.empty())
        collect_entries(dir, entries);
    std::sort(entries.begin(), entries.end(), heavier_first);
    return entries;
}

std::string read_library_text(const std::string& path) {
    return read_text(path);
}

std::string render_working_index() {
    const std::vector<std::string> wanted = selected_tags_from(index_store::read_all());
    const auto entries = library_entries();
    if (entries.empty())
        return {};

    // The craft layer answers what the task needs, so it is indexed for every
    // project; the project's own tags add the domain entries. A fresh project
    // has no tags and still sees the craft layer.
    std::vector<LibraryEntry> selected;
    for (const auto& entry : entries)
        if (!entry.trigger.empty() && entry_matches(entry, craft_tags()))
            selected.push_back(entry);
    if (!wanted.empty())
        for (const auto& entry : entries)
            if (!entry.trigger.empty() && !entry_matches(entry, craft_tags()) &&
                entry_matches(entry, wanted))
                selected.push_back(entry);
    if (selected.empty())
        return {};

    std::string out = "## Library entries this project selects\n\n"
                      "Trigger lines only: ask `library_lookup` for the entry by name when the "
                      "situation matches. The craft layer (design, patterns, architecture) is "
                      "always listed; the project's tags add the rest.\n\n";
    const std::size_t shown = std::min(selected.size(), kMaxEntries);
    for (std::size_t i = 0; i < shown; ++i)
        append_line(out, selected[i]);
    if (selected.size() > shown) {
        out += "\n(";
        out += std::to_string(selected.size() - shown);
        out += " more selected entries — query `library_lookup`)\n";
    }
    return out;
}

std::string render_library_skill_body() {
    std::string out =
        "# Amber's coding library\n\n"
        "The library is reference material: one concept per entry, written to be "
        "read when the situation matches. The working index names the entries this "
        "project selects; this is how to reach the rest.\n\n"
        "## Reaching an entry\n\n"
        "- `library_lookup(name=\"<entry>\")` — the full text of an entry the working "
        "index names.\n"
        "- `library_lookup(query=\"<words>\")` — entries matching a situation. The "
        "words people use work: OOP, SOLID, patterns, architecture, sanitizer, "
        "bisect.\n"
        "- `library_lookup(list=true)` — the vocabulary and every entry name.\n\n"
        "## Vocabulary\n\n";
    const std::string dir = library_dir();
    if (!dir.empty()) {
        std::ifstream in(fs::path(dir) / kVocabulary);
        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = trim(line);
            if (trimmed.rfind("- `", 0) == 0) {
                out += trimmed;
                out += '\n';
            }
        }
    }
    return out;
}

std::string render_library_list() {
    std::string out = render_library_skill_body();
    out += "\n## Entries\n\n";
    for (const auto& entry : library_entries()) {
        out += "- ";
        out += entry.name;
        out += ": ";
        out += entry.trigger;
        out += '\n';
        if (!entry.topic)
            continue;
        for (const auto& child : topic_entries(fs::path(entry.path).parent_path().string())) {
            out += "  - ";
            out += child.name;
            out += ": ";
            out += child.trigger;
            out += '\n';
        }
    }
    return out;
}

} // namespace agent::plugins
