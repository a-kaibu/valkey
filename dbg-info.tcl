set fd [socket 127.0.0.1 7786]
fconfigure $fd -translation binary -buffering full
proc pack {args} {
    set o "*[llength $args]\r\n"
    foreach a $args { append o "\$[string length $a]\r\n$a\r\n" }
    return $o
}
puts -nonewline $fd [pack COMMAND INFO ZVUNION]
flush $fd
set raw ""
while {[gets $fd line] >= 0} {
    append raw $line "\n"
    if {[string match "*-slow*" $line]} break
}
puts $raw
