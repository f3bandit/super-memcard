#!/bin/sh
# Super MemCard phase 2: puts the front-port card driver (core/sys/smc_sys.sv) into a core's sys/, so Main reaches the
# card while that core runs. Only sys_top.v changes, at four lines every sys/ version has (checked on all 15 pinned
# cores); the core's own code doesn't. A script rather than a .patch because the cores carry 4 different sys_top.v.
#   tools/sys-patch.sh <core folder>      (tools/cores.sh patch runs it)
# Copyright (C) 2026 f3bandit.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
sys=${1:?usage: sys-patch.sh <core folder>}/sys
top=$sys/sys_top.v
[ -f "$top" ] || { echo "no $top"; exit 1; }
grep -q smc_sys "$top" && { echo "$top: already has Super MemCard"; exit 0; }

for f in smc_sys smc_port smc_card_sector smc_sio_byte; do cp "$here/core/sys/$f.sv" "$sys/"; done
cp "$here/core/sys/smc_sys.sdc" "$sys/"
for f in smc_sys smc_port smc_card_sector smc_sio_byte; do
	printf 'set_global_assignment -name SYSTEMVERILOG_FILE [file join $::quartus(qip_path) %s.sv ]\n' "$f" >> "$sys/sys.qip"
done
printf 'set_global_assignment -name SDC_FILE           [file join $::quartus(qip_path) smc_sys.sdc ]\n' >> "$sys/sys.qip"

perl -0pi -e '
	my $n = 0;
	# 1. its answers next to sys_top'"'"'s own
	$n += s/(\nwire \[31:0\] gp_in = )([^\n]*)io_dout \| io_dout_sys\}/\n\/\/ Super MemCard (super-memcard core\/sys\/smc_sys.sv): the front ports for Main while the core runs\nwire [15:0] smc_dout;\nwire        smc_on;\nwire  [6:0] smc_user_out, user_out_s;\n$1$2io_dout | io_dout_sys | smc_dout}/;
	# 2. the user port: the core'"'"'s, unless Main has turned the front ports on
	$n += (s/(assign USER_IO\[[0-6]\] = [^\n]*)user_out\[/$1user_out_s[/g == 7);
	# 3. the module, on the core'"'"'s clock with the UIO commands
	$n += s/(\nwire  \[6:0\] user_out, user_in;\n)/$1\nassign user_out_s = smc_on ? smc_user_out : user_out;\nsmc_sys smc_sys\n(\n\t.clk_sys(clk_sys),\n\t.clk_50(FPGA_CLK3_50),\n\t.reset(reset),\n\t.io_uio(io_uio),\n\t.io_strobe(io_strobe),\n\t.io_din(io_din),\n\t.io_dout(smc_dout),\n\t.ports_on(smc_on),\n\t.user_out(smc_user_out),\n\t.user_in(user_in)\n);\n/;
	die "sys_top.v: only $n of 3 places found\n" unless $n == 3;
' "$top"
echo "$top: Super MemCard front ports added"
