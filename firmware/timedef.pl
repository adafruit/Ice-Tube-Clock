#!/usr/bin/perl

@timeData = localtime(time);
print "-DTIMESEC=$timeData[0] -DTIMEMIN=$timeData[1] -DTIMEHOUR=$timeData[2]"; 
