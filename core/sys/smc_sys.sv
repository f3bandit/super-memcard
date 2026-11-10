// Super MemCard phase 2: the SuperStation One's front ports reachable from Main while a game core runs.
// Copyright (C) 2026 f3bandit. GPL-3.0-or-later.
//
// Lives in sys_top (tools/sys-patch.sh adds it to every core's sys/), so no core changes: Main sends the same
// commands as in a menu core (smc_port.sv: SMC_ID 80 .. SMC_RAW_STATUS 88) on the UIO channel,
// and they're answered here next to sys_top's own. hps_io doesn't use 80-88 and the cores' own EXT_BUS users answer
// 34/35, so nothing else answers them.
//
// The front ports stay the core's until Main turns them on (SMC_MODE) for a card frame; until then USER_OUT is the
// core's own, so a core using SNAC (the PSX core) is never disturbed. Main only turns them on in cores that don't
// (05-design.md, phase 2). Raw card frames only (PADS 0, RAW_ONLY 1): no controller reads or sector commands, so it
// fits next to big cores. Main's input comes from USB as always.
//
// sys_top's clk_sys is the core's clock, which differs per core: it's measured against the 50 MHz board clock, so
// the bus has a PS1's timing in every core.

module smc_sys
(
	input         clk_sys,
	input         clk_50,      // FPGA_CLK3_50: the reference
	input         reset,

	input         io_uio,
	input         io_strobe,
	input  [15:0] io_din,
	output [15:0] io_dout,     // OR'd into sys_top's io_dout (0 unless it's ours)

	output        ports_on,    // the user port is the front ports' now
	output  [6:0] user_out,    // the front ports' pins on the user port
	input   [6:0] user_in
);

// clk_sys cycles in 64 us (3200 cycles of 50 MHz)
reg [11:0] ref_cnt = 0;
reg        tick = 0;
always @(posedge clk_50) begin
	if (ref_cnt == 12'd3199) begin ref_cnt <= 0; tick <= ~tick; end
	else ref_cnt <= ref_cnt + 1'd1;
end

(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED_IF_ASYNCHRONOUS" *) reg [2:0] tick_s = 0;
reg [15:0] cnt = 0, u64 = 0;
always @(posedge clk_sys) begin
	tick_s <= {tick_s[1:0], tick};
	if (tick_s[2] ^ tick_s[1]) begin
		u64 <= cnt;
		cnt <= 16'd1;
	end
	else if (~&cnt) cnt <= cnt + 1'd1;
end

wire [15:0] dout;
wire        dout_en;
wire        sel1_n, sel2_n, sclk, scmd;

smc_port #(.PADS(0), .RAW_ONLY(1)) smc_port
(
	.clk(clk_sys),
	.reset(reset),
	.clk_u64(u64),
	.io_strobe(io_strobe),
	.io_enable(io_uio),
	.io_din(io_din),
	.io_dout(dout),
	.dout_en(dout_en),
	.ports_on(ports_on),
	.show_static(),
	.sio_sel1_n(sel1_n),
	.sio_sel2_n(sel2_n),
	.sio_clk(sclk),
	.sio_cmd(scmd),
	.sio_dat(user_in[4]),
	.sio_ack(user_in[3])
);

assign io_dout  = dout_en ? dout : 16'd0;
assign user_out = {1'b1, sclk, 2'b11, scmd, sel1_n, sel2_n};

endmodule
