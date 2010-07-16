#!perl # -*- perl -*-
# 
# Author: Christopher Browne
# Copyright 2004 Afilias Canada

require 'slon-tools.pm';
require 'slon.env';

my $FILE="/tmp/restart.$$";
foreach my $node (@NODES) {
  my $dsn = $DSN[$node];
  open(SLONIK, ">$FILE");
  print SLONIK qq{
	cluster name = $SETNAME ;
	node $node admin conninfo = '$dsn';
	restart node $node;
    };
  close SLONIK;
  run_slonik_script($FILE);
}
