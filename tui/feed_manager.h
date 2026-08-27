#ifndef AMBER_TUI_FEED_MANAGER_H
#define AMBER_TUI_FEED_MANAGER_H

namespace tui {
class Tui;

class FeedManager {
public:
    explicit FeedManager(Tui& tui);
    void refresh_model_list();
    void refresh_policy_feed();
    void refresh_provider_feed();
    void refresh_job_feed();

private:
    Tui& tui_;
};

} // namespace tui

#endif
