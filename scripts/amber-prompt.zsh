
# amber shell prompt — single line, project-aware, git-aware
#
# Source from .zshrc (adjust path to your checkout):
#   source /home/jack/Projects/cpp-agent/scripts/amber-prompt.zsh
#
# Or from any directory:
#   source <path-to-amber>/scripts/amber-prompt.zsh
#
# Configurable via AMBER_PROMPT env:
#   AMBER_PROMPT=minimal   →  fix/ui-fix  +3/-1
#   AMBER_PROMPT=full      →  ┌─(amber)─fix/ui-fix─(+3/-1)─
#                           ╰─
#   AMBER_PROMPT=deluxe    →  amber fix/ui-fix +3/-1 ❯
#
# Default: deluxe (single-line, compact)
#
# Quick test (run from the project root):
#   source scripts/amber-prompt.zsh

_amber_prompt() {
    local project="amber"
    local branch git_dir

    # Get current git branch (fast, no fs call if not in git dir)
    if git_dir="$(git rev-parse --show-toplevel 2>/dev/null)"; then
        branch="${"$(git symbolic-ref HEAD 2>/dev/null)"#refs/heads/}"
        # Short stat: +added/-deleted lines
        local stat
        stat=$(git diff --shortstat 2>/dev/null)
        local ins=0 del=0
        if [[ "$stat" =~ ([0-9]+)\ insertion ]]; then ins=$match[1]; fi
        if [[ "$stat" =~ ([0-9]+)\ deletion ]];  then del=$match[1]; fi

        local style="${AMBER_PROMPT:-deluxe}"

        case "$style" in
            minimal)
                PROMPT="%F{cyan}${branch}%f"
                [[ $ins -gt 0 || $del -gt 0 ]] && PROMPT+="  %F{green}+${ins}%f/%F{red}-${del}%f"
                PROMPT+=" %# "
                ;;
            full)
                PROMPT="┌─(%F{yellow}${project}%f)─%F{cyan}${branch}%f"
                [[ $ins -gt 0 || $del -gt 0 ]] && PROMPT+="─(%F{green}+${ins}%f/%F{red}-${del}%f)"
                PROMPT+="─"$'\n'"╰$%f "
                ;;
            deluxe|*)
                # Compact single-line:  amber  fix/ui-fix  +3/-1  ❯
                local p="%F{yellow}${project}%f "
                p+="%F{cyan}${branch}%f"
                [[ $ins -gt 0 || $del -gt 0 ]] && p+="  %F{green}+${ins}%f/%F{red}-${del}%f"
                p+=" %F{white}❯%f "
                PROMPT="$p"
                ;;
        esac
    else
        # Not in a git repo — simple prompt
        PROMPT="%F{yellow}${project}%f %# "
    fi
}

precmd_functions+=(_amber_prompt)
