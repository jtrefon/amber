#include "tui/skills_detail_view.h"

#include "agent/remote_text.h"
#include "agent/skill_catalog.h"
#include "tui/markdown.h"
#include "tui/rich_paint.h"
#include "tui/skills_format.h"

#include <algorithm>

namespace tui {

namespace {

std::size_t first_previewable(const std::vector<agent::SkillAsset>& assets) {
    for (std::size_t i = 0; i < assets.size(); ++i) {
        if (previewable_asset(assets[i].path))
            return i;
    }
    return 0;
}

const std::string* bytes_for(const agent::SkillDetail& detail,
                             const std::map<std::string, std::string>* files,
                             const std::string& path) {
    if (files) {
        const auto it = files->find(path);
        if (it != files->end())
            return &it->second;
    }
    if (path == "SKILL.md")
        return &detail.body;
    return nullptr;
}

void fill_body(DetailView& view, const agent::SkillDetail& detail,
               const std::map<std::string, std::string>* files) {
    if (view.assets.empty())
        return;
    const std::string& path = view.assets[view.file].path;
    if (!previewable_asset(path)) {
        view.note = "not previewed: not a text file";
        return;
    }
    const std::string* bytes = bytes_for(detail, files, path);
    if (!bytes) {
        view.note = "loading " + path + "...";
        return;
    }
    view.body = md::render(*bytes);
}

void draw_wrapped(WINDOW* w, int row, int left, int width, const std::string& text, int attr) {
    if (text.empty())
        return;
    wattron(w, attr);
    mvwaddnstr(w, row, left, text.c_str(), std::max(0, width));
    wattroff(w, attr);
}

int draw_blocks(WINDOW* w, int y, int left, int width, const DetailView& view) {
    draw_wrapped(w, y++, left, width, view.name, A_BOLD);
    draw_wrapped(w, y++, left, width, view.source, A_DIM);
    draw_wrapped(w, y++, left, width, view.headline, 0);
    mvwaddnstr(w, y++, left, std::string(static_cast<std::size_t>(width), '-').c_str(), width);
    for (const auto& line : view.evidence)
        draw_wrapped(w, y++, left, width, line, 0);
    if (!view.audit.empty())
        draw_wrapped(w, y++, left, width, "Audit  " + view.audit, 0);
    if (!view.cost.empty())
        draw_wrapped(w, y++, left, width, "Cost   " + view.cost, 0);
    return y;
}

std::string file_header(const DetailView& view) {
    if (view.assets.empty())
        return "SKILL.md preview";
    const agent::SkillAsset& asset = view.assets[view.file];
    return asset.path + "  " + human_size(static_cast<long long>(asset.bytes)) + "  [" +
           std::to_string(view.file + 1) + "/" + std::to_string(view.assets.size()) + "]";
}

void draw_body(WINDOW* w, const DetailRect& rect, int y, const DetailView& view, int scroll) {
    if (view.body.empty()) {
        if (!view.note.empty())
            draw_wrapped(w, y, rect.left, rect.width, view.note, A_DIM);
        return;
    }
    const int rows = std::max(0, rect.top + rect.height - y);
    const int first = std::clamp(scroll, 0, std::max(0, static_cast<int>(view.body.size()) - 1));
    for (int i = 0; i < rows; ++i) {
        const auto index = static_cast<std::size_t>(first) + static_cast<std::size_t>(i);
        if (index >= view.body.size())
            break;
        rich::paint_line(w, y + i, std::max(0, rect.width), view.body[index]);
    }
}

} // namespace

DetailView build_detail_view(const agent::SkillDetail& detail, const DetailInstall& install,
                             int body_budget_tokens,
                             const std::map<std::string, std::string>* files,
                             std::size_t file_index) {
    DetailView view;
    view.id = detail.ref.id;
    view.name = detail.ref.name;
    view.source = detail.ref.source;
    const int risks = static_cast<int>(agent::scan_skill_body(detail.body).size());
    const DetailBlocks blocks = detail_blocks(detail, install, body_budget_tokens, risks);
    view.headline = blocks.headline;
    view.evidence = blocks.evidence;
    view.audit = blocks.audit;
    view.cost = blocks.cost;
    view.assets = detail.assets;
    view.file = std::min(file_index, view.assets.empty() ? 0 : view.assets.size() - 1);
    if (!view.assets.empty() && !previewable_asset(view.assets[view.file].path))
        view.file = first_previewable(view.assets);
    fill_body(view, detail, files);
    return view;
}

void draw_detail_pane(WINDOW* w, const DetailRect& rect, const DetailView& view, int scroll) {
    if (rect.width < 12 || rect.height < 3)
        return;
    int y = draw_blocks(w, rect.top, rect.left, rect.width, view);
    if (y >= rect.top + rect.height)
        return;
    mvwaddnstr(w, y++, rect.left, std::string(static_cast<std::size_t>(rect.width), '-').c_str(),
               rect.width);
    draw_wrapped(w, y++, rect.left, rect.width, file_header(view), A_DIM);
    draw_body(w, rect, y, view, scroll);
}

} // namespace tui
