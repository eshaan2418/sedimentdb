# Regenerate with: make build && vhs demo.tape
Output docs/demo.gif
Set Shell "bash"
Set FontSize 14
Set Width 1250
Set Height 560
Set TypingSpeed 25ms
Set Theme "Builtin Dark"
Hide
Type "export PS1='$ ' && clear" Enter
Show
Type "./build/sediment-crashtest --rounds 8 --dir /tmp/demo-crash" Enter
Sleep 7s
Type "./build/db_bench --engine=sediment --benchmarks=fillrandom,readrandom,readmissing --num=300000 --db=/tmp/demo-db" Enter
Sleep 6s
