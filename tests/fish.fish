function orig -d "the original"; echo "orig $argv"; end
functions --copy orig copy
function orig; echo replaced; end
copy x; orig
bind \cg 'commandline -r hi'; bind up history-search-backward; bind alt-x forward-char
bind
bind -e up; bind | count
commandline -r nope 2>/dev/null; echo status=$status
