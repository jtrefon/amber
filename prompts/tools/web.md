# Web access

`web_fetch` reads an `http(s)` URL and returns extracted text. `web_search`
returns titles, URLs and snippets for a query. Both are read-only and require no
browser.

The external `cdp` plugin is a different provider: it drives a live Chrome page
for JavaScript, clicks, typing and screenshots. Select one with
`/set web provider builtin|cdp|off`. `cdp` requires the `cdp` plugin to be
active; `off` disables the built-in provider. When the provider is `cdp` or
`off`, the built-in tools fail closed instead of silently reaching the network.
