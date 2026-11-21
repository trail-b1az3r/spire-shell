a=(one two three); echo ${a[@]} ${a[1]} ${#a[@]} ${a[-1]} $a[1]
a+=(four); a[0]=ONE; echo ${a[@]}
s=ab; s+=cd; echo $s
set w x; echo $w[2..-1] end
