case "$TERM" in
    xterm) export TERM=xterm-256color;;
esac

if [ -x /usr/bin/dircolors ]; then
    alias ls='ls --color=auto'
    alias grep='grep --color=auto'
fi

alias ll='ls -alF'

export PS1="\[\e[34;1m\][\t]\[\e[0m\] \w \[\e[34;1m\]#\[\e[0m\] "

shopt -q login_shell && echo "Welcome to the Pedigree Operating System.

Run 'tour' for a quick introduction to Pedigree."
