# Taste

- The user uses only the TUI version of the tool, not the CLI ("i dont use cli only tui version"). The TUI is the primary product surface, so fixes and verification should target the TUI behavior — do not assume or weigh CLI behavior/tests as the user's actual usage. Confidence: 0.85

- Tends to frame distinct problems as separate issues and expects each addressed on its own (e.g., the `-v` version flag vs. the slash-command JSON being misplaced/undeployed are different problems, not one). Confidence: 0.7

- API keys/tokens must be entered through a masked TUI/Ncurses dialog field, never typed into the chat/prompt input line, so credentials never end up in session logs/transcripts or get sent to the LLM ("we need ability to add key using TUI/Ncurses window so it never ends in logs or is sent anywhere"). Secret capture is a security surface, not a chat input. Confidence: 0.85

- When a key is needed mid-session (HTTP 401/403 on the active provider), prefers a focused masked single-field key dialog shown at the point of failure — chosen over opening the full multi-field provider editor or a message-only hint. The prompt must be cancellable (decline → the turn degrades gracefully, no deadlock). Confidence: 0.7

- A `/set provider <name>` switch to a key-requiring provider with no key must prompt inline for the key rather than switching with only a warning that strands the agent unable to authenticate. Key entry hidden two levels deep in `/settings` is treated as "not working": the obvious slash command is where the capability must surface (a "we had it, now I don't see it working" report can be a reachability gap, not removed code). Confidence: 0.75

- Live operational/account state belongs on the status bar: when asking for a new readout (e.g., "display on our status bar, current kilo balance"), explicitly requests it there rather than a separate screen/panel — the status bar is treated as the canonical home for at-a-glance counters alongside the context gauge. Confidence: 0.55

- Status-bar counters (token/byte counts) must use conventional magnitude abbreviations: 1,000,000 renders as "1m" (or "1mln") — compound forms like "1000k" are treated as nonsense ("1000k is not even a thing"). Values should compress to the standard unit rather than stacking the previous tier. Confidence: 0.55

- Status-bar badges are judged against observable runtime behavior, not the config value they mirror: when reasoning/thinking are set but the bar still shows "reasoning off" while the user can see the model actually reasoning (dark-gray reasoning text), the display is called broken — the readout is expected to reflect the effective applied state, and a divergence between what a `/set` command writes and what the engine/agent really uses is a real bug (e.g., a handler that only mutates the UI's copy), not a cosmetic quirk. Confidence: 0.6