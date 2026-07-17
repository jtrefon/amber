# ncurses Reference for amber

Quick reference for ncurses functions used in this project.

## Initialization
```
initscr()                - initialize curses
cbreak()                 - char-at-a-time input, no line buffering
noecho()                 - don't echo input
keypad(win, TRUE)        - enable function/arrow keys
start_color()            - enable color support
use_default_colors()     - allow -1 (default terminal color) in init_pair
set_escdelay(N)          - ESC key response delay in ms
curs_set(0|1|2)          - hide/show cursor
timeout(N)               - blocking getch timeout in ms (0=nonblock, -1=block)
```

## Windows & Panels
```
newwin(h, w, y, x)       - create window
delwin(win)              - delete window
derwin(win, h, w, y, x)  - create subwindow (relative coords)
box(win, vert, horz)     - draw border (ACS_VLINE, ACS_HLINE)
wbkgd(win, ch)           - set background attribute
getmaxyx(win, y, x)      - get window dimensions
keypad(win, TRUE)        - enable special keys
```

## Panels (overlapping windows)
```
new_panel(win)           - create panel from window
del_panel(panel)         - delete panel
update_panels()          - compute panel stacking order
top_panel(panel)         - bring panel to top
```

## Output
```
addch(ch)                - write char to stdscr
addstr(str)              - write string to stdscr
mvwaddch(win, y, x, ch)  - move + write char
mvwaddstr(win, y, x, s)  - move + write string
mvwaddnstr(win, y, x, s, n) - write at most n chars
printw(fmt, ...)         - printf-style output to stdscr
mvwprintw(win, y, x, fmt, ...) - printf-style to window
refresh()                - update screen from stdscr
wnoutrefresh(win)        - mark window for output
doupdate()               - flush all pending refreshes
erase()                  - clear screen
clrtoeol()               - clear to end of line
hline(ch, n)             - draw horizontal line (ACS_HLINE)
vline(ch, n)             - draw vertical line (ACS_VLINE)
```

## Colors
```
init_pair(n, fg, bg)     - define colour pair n (1..COLOR_PAIRS-1)
COLOR_PAIR(n)            - attribute for colour pair n
start_color()            - enable colour support
init_color(n, r, g, b)   - redefine colour n (values 0..1000)
COLORS                   - max colours available
COLOR_PAIRS              - max pairs available
```

## ACS line drawing constants
```
ACS_ULCORNER    ┌    ACS_URCORNER    ┐
ACS_LLCORNER    └    ACS_LRCORNER    ┘
ACS_HLINE       ─    ACS_VLINE       │
ACS_LTEE        ├    ACS_RTEE        ┤
ACS_TTEE        ┴    ACS_BTEE        ┬
ACS_PLUS        ┼
```

## Input constants
```
KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT  - arrow keys
KEY_NPAGE, KEY_PPAGE                    - page up/down
KEY_ENTER, KEY_HOME, KEY_END           - special keys
KEY_DC                                 - delete key
KEY_BTAB                               - back-tab
KEY_BACKSPACE                          - backspace
```

## Forms library (-lformw)
```
new_field(h, w, top, left, offok, nbufs) - create a form field
set_field_back(f, attr)                  - set field background attr
set_field_fore(f, attr)                  - set field foreground attr
field_opts_off(f, opts)                  - disable field options
set_field_buffer(f, buf, str)            - set field content
new_form(fields)                         - create form
set_form_win(form, win)                  - set form's window
set_form_sub(form, sub)                  - set form's sub-window
post_form(form)                          - display form
unpost_form(form)                        - hide form
free_form(form)                          - free form
free_field(field)                        - free field
form_driver(form, c)                     - process key in form
set_current_field(form, field)           - focus a field
field_index(field)                       - get field index
```

## Menu library (-lmenuw)
```
new_item(name, desc)                     - create menu item
new_menu(items)                          - create menu
set_menu_win(menu, win)                  - set menu's window
set_menu_sub(menu, sub)                  - set menu's subwindow
set_menu_mark(menu, str)                 - selection marker
set_menu_format(menu, rows, cols)        - menu grid
menu_opts_off(menu, opts)               - disable options
post_menu(menu)                          - display menu
unpost_menu(menu)                        - hide menu
free_menu(menu)                          - free menu
free_item(item)                          - free item
menu_driver(menu, c)                     - process key
item_index(item)                         - item position
current_item(menu)                       - currently selected item
```

## Common pitfalls (from this project)
- `box(win, 0, 0)` uses ACS_VLINE/ACS_HLINE — safe with ncursesw + UTF-8 locale
- `wbkgd` with a colour pair also sets the background `chtype`; subsequent writes
  inherit the pair unless overridden
- Panels only work via `update_panels()` + `doupdate()`, not `refresh()`/`wnoutrefresh()`
- `getmaxyx` is a macro, not a function
- ACS constants may render as boxes on non-UTF-8 locales; `setlocale(LC_ALL, "")` fixes this
