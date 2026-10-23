#include "web_parse.h"

#include <cctype>
#include <regex>
#include <sstream>

namespace agent::plugins::web {

namespace {

std::string lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool is_block_tag(const std::string& tag) {
    return tag == "p" || tag == "div" || tag == "li" || tag == "tr" || tag == "br" || tag == "h1" ||
           tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6";
}

std::string collapse_whitespace(const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == '\n') {
            if (!out.empty() && out.back() != '\n')
                out.push_back('\n');
            space = false;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
        } else {
            if (space && !out.empty() && out.back() != '\n')
                out.push_back(' ');
            out.push_back(c);
            space = false;
        }
    }
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

std::string decode_numeric(const std::string& body) {
    try {
        std::string digits = body;
        if (!digits.empty() && digits[0] == '#')
            digits.erase(digits.begin());
        const int value = digits.size() > 1 && (digits[1] == 'x' || digits[1] == 'X')
                              ? std::stoi(digits.substr(2), nullptr, 16)
                              : std::stoi(digits);
        if (value <= 0 || value > 0x10ffff)
            return {};
        std::string out;
        if (value < 0x80) {
            out.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (value >> 6)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (value >> 12)));
            out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string percent_decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (text[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string result_url(std::string url) {
    const std::regex redirect(R"([?&]uddg=([^&]+))");
    std::smatch match;
    if (std::regex_search(url, match, redirect))
        url = percent_decode(match[1].str());
    if (url.rfind("//", 0) == 0)
        url = "https:" + url;
    return decode_entities(url);
}

} // namespace

bool safe_web_url(const std::string& url, std::string& error) {
    const std::string lowered = lower(url);
    if (lowered.rfind("http://", 0) != 0 && lowered.rfind("https://", 0) != 0) {
        error = "only http:// and https:// URLs are allowed";
        return false;
    }
    for (unsigned char c : url) {
        if (std::iscntrl(c) || std::isspace(c)) {
            error = "URL contains whitespace or control characters";
            return false;
        }
    }
    const std::size_t scheme = url.find("://") + 3;
    const std::size_t authority_end = url.find_first_of("/?#", scheme);
    const std::string authority = url.substr(scheme, authority_end - scheme);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        error = "URL must have a host and no embedded credentials";
        return false;
    }
    return true;
}

std::string url_encode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string decode_entity(const std::string& body) {
    if (body == "amp")
        return "&";
    if (body == "lt")
        return "<";
    if (body == "gt")
        return ">";
    if (body == "quot")
        return "\"";
    if (body == "apos" || body == "#39")
        return "'";
    if (body == "nbsp")
        return " ";
    if (!body.empty() && body[0] == '#')
        return decode_numeric(body);
    return {};
}

std::string decode_entities(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }
        const std::size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back(text[i++]);
            continue;
        }
        const std::string replacement = decode_entity(text.substr(i + 1, semi - i - 1));
        if (replacement.empty())
            out.append(text, i, semi - i + 1);
        else
            out += replacement;
        i = semi + 1;
    }
    return out;
}

std::string html_to_text(const std::string& html) {
    std::string text;
    bool skip = false;
    for (std::size_t i = 0; i < html.size();) {
        if (html[i] != '<') {
            if (!skip)
                text.push_back(html[i]);
            ++i;
            continue;
        }
        std::size_t end = html.find('>', i + 1);
        if (end == std::string::npos)
            break;
        std::string tag = html.substr(i + 1, end - i - 1);
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.erase(tag.begin());
        std::size_t stop = tag.find_first_of(" \t\r\n/");
        tag = lower(tag.substr(0, stop));
        if (tag == "script" || tag == "style")
            skip = !closing;
        else if (!skip && !closing && is_block_tag(tag))
            text.push_back('\n');
        i = end + 1;
    }
    return collapse_whitespace(decode_entities(text));
}

std::vector<SearchResult> parse_search_results(const std::string& html) {
    const std::regex link(
        R"re(<a[^>]*class="[^"]*result__a[^"]*"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>)re",
        std::regex::icase);
    const std::regex snippet(R"re(<a[^>]*class="[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)</a>)re",
                             std::regex::icase);
    std::vector<std::string> snippets;
    for (std::sregex_iterator it(html.begin(), html.end(), snippet), end; it != end; ++it)
        snippets.push_back(html_to_text((*it)[1].str()));

    std::vector<SearchResult> out;
    std::size_t index = 0;
    for (std::sregex_iterator it(html.begin(), html.end(), link), end; it != end; ++it, ++index) {
        SearchResult result;
        result.url = result_url((*it)[1].str());
        result.title = html_to_text((*it)[2].str());
        if (index < snippets.size())
            result.snippet = snippets[index];
        if (!result.url.empty() && !result.title.empty())
            out.push_back(std::move(result));
    }
    return out;
}

} // namespace agent::plugins::web
