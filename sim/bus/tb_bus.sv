// Layer 2 of the virtual SS1 (docs/07): Phosphor's front-port logic (phosphor-menu phosphor_ext, card_sector,
// sio_byte) driven the way Main drives it over EXT_BUS, with the virtual card, sd2psXtd's own PS1 code with Super
// MemCard, on front port 1 bit by bit (vcard_bus + vcardd). Port 2 is empty.
// Run: sh sim/bus/run.sh
// Copyright (C) 2026 f3bandit.
`timescale 1ns/1ps

module tb;
	// 100 MHz like the menu core; -DCLK_MHZ=50 (or any) as a game core's clock, where sys_top runs the same extension
	// with its measured clock (phase 2)
`ifndef CLK_MHZ
`define CLK_MHZ 100
`endif
	reg clk = 0;
	always #(500.0 / `CLK_MHZ) clk = ~clk;
	localparam integer CLK_U64 = $rtoi(64.0 * `CLK_MHZ);   // clk cycles in 64 us
	reg reset = 1;

	reg        io_strobe = 0, io_enable = 0;
	reg [15:0] io_din = 0;
	wire [15:0] io_dout;
	wire       dout_en, ports_on, show_static;
	wire       sel1_n, sel2_n, sclk, scmd, dat1, ack1;

`ifdef SMC_SYS
	// phase 2: the module sys_top carries in every game core (core/sys/smc_sys.sv), its clock measured against a 50 MHz
	// reference, the front ports on the user port's pins as sys_top drives them
	reg clk50 = 0;
	always #10 clk50 = ~clk50;
	wire [6:0] uo;
	wire [15:0] sys_dout;
	assign dout_en = 1'b1;   // smc_sys gives 0 for anything not its own
	assign io_dout = sys_dout;
	assign {sclk, scmd, sel1_n} = {uo[5], uo[2], uo[1]};
	smc_sys dut
	(
		.clk_sys(clk), .clk_50(clk50), .reset(reset),
		.io_uio(io_enable), .io_strobe(io_strobe), .io_din(io_din), .io_dout(sys_dout),
		.ports_on(ports_on), .user_out(uo), .user_in({2'b11, dat1, ack1, 3'b111})
	);
	assign sel2_n = uo[0];
`else
	// controllers are read rarely here: each poll of port 1 reaches the virtual card, which takes real time
	phosphor_ext #(.GAP(23'h7FFFFF)) dut
	(
		.clk(clk), .reset(reset), .clk_u64(16'(CLK_U64)),
		.io_strobe(io_strobe), .io_enable(io_enable), .io_din(io_din), .io_dout(io_dout), .dout_en(dout_en),
		.ports_on(ports_on), .show_static(show_static),
		.sio_sel1_n(sel1_n), .sio_sel2_n(sel2_n), .sio_clk(sclk), .sio_cmd(scmd),
		.sio_dat(dat1), .sio_ack(ack1)
	);
`endif

	vcard_bus card(.sel_n(sel1_n), .sclk(sclk), .cmd(scmd), .dat(dat1), .ack_n(ack1));

	// ---- Main's side (as phosphor-main does it) ----
	reg [15:0] r;
	task automatic w(input [15:0] v, output [15:0] got);
		begin
			@(negedge clk) io_din = v; io_strobe = 1;
			@(negedge clk) io_strobe = 0;
			repeat (20) @(negedge clk);
			got = dout_en ? io_dout : 16'h0000;
		end
	endtask
	task automatic begin_cmd(input [15:0] c);
		begin @(negedge clk) io_enable = 1; repeat (4) @(negedge clk); w(c, r); end
	endtask
	task automatic end_cmd;
		begin @(negedge clk) io_enable = 0; repeat (4) @(negedge clk); end
	endtask

	integer errors = 0;
	// the bit clock on the wire: the shortest time between SCLK edges since min_reset (gaps between bytes are longer)
	realtime t_edge = 0, min_half = 1e9;
	always @(sclk) begin if ($realtime - t_edge < min_half) min_half = $realtime - t_edge; t_edge = $realtime; end
	task min_reset; begin min_half = 1e9; t_edge = $realtime; end endtask
	task check_half(input [255:0] what, input real want_ns);
		begin
			if (min_half < want_ns * 0.95 || min_half > want_ns * 1.05) begin
				$display("FAIL %0s: half bit %0.0f ns, want %0.0f", what, min_half, want_ns); errors = errors + 1;
			end else $display("ok   %0s (%0.0f ns at %0.2f MHz)", what, min_half, 1.0 * `CLK_MHZ);
		end
	endtask

	task check(input [255:0] what, input [31:0] got, input [31:0] want);
		begin
			if (got !== want) begin $display("FAIL %0s: got %h want %h", what, got, want); errors = errors + 1; end
			else $display("ok   %0s", what);
		end
	endtask

	// raw frames: fr[] out, then the card's bytes back in fr[]; ack wait in units of 1024 clk (9766 = 100 ms)
	reg  [7:0] fr[0:255];
	reg [15:0] rst, rw16;
	integer    fl;   // frame length being built
	reg  [1:0] spd = 2'd2;   // Super MemCard frames' speed (PH_RAW, version 3): 1 MHz, as Phosphor's Main sends them
	task automatic raw_frame(input integer port, input integer len);
		integer k, tries;
		begin
			begin_cmd(16'h87);
			w({7'd0, port[0], len[7:0]}, r);
			w({(fr[1] == 8'h60 || fr[1] == 8'h52) ? 2'd0 : spd, 14'd9766}, r);   // Hello and PS1 frames as a PS1; Super MemCard at spd
			for (k = 0; k < len; k = k + 2) w({(k + 1 < len) ? fr[k + 1] : 8'h00, fr[k]}, r);
			end_cmd;
			tries = 0;
			do begin #(100_000); begin_cmd(16'h88); rst = r; end_cmd; tries = tries + 1; end while (rst[15] && tries < 5000);
			begin_cmd(16'h88); rst = r;
			for (k = 0; k < len; k = k + 2) begin w(0, rw16); fr[k] = rw16[7:0]; if (k + 1 < len) fr[k + 1] = rw16[15:8]; end
			end_cmd;
		end
	endtask
	task automatic put(input [7:0] b); begin fr[fl] = b; fl = fl + 1; end endtask
	task automatic start_frame(input [7:0] c); begin fl = 0; put(8'h81); put(c); end endtask
	task automatic put_str(input [8*16-1:0] s, input integer n);   // n characters (a string literal is right-aligned in s)
		integer k; begin put(n[7:0]); for (k = 0; k < n; k = k + 1) put(s[(n - 1 - k) * 8 +: 8]); end
	endtask
	task automatic zeros(input integer n); integer k; begin for (k = 0; k < n; k = k + 1) put(8'h00); end endtask

	// PS1 sector read through PH_CARD (the existing path), up to 3 tries
	reg [15:0] data[0:63];
	task automatic ps1_read(input [9:0] sec);
		integer k, n, t;
		begin
			for (t = 0; t < 3; t = t + 1) begin
				begin_cmd(16'h83); w(16'h0000, r); w({6'd0, sec}, r); end_cmd;
				n = 0;
				do begin #(500_000); begin_cmd(16'h84); rst = r; end_cmd; n = n + 1; end while (rst[15] && n < 200);
				if (rst == 16'h4000) t = 3;
			end
			begin_cmd(16'h84); rst = r; for (k = 0; k < 64; k = k + 1) begin w(0, rw16); data[k] = rw16; end end_cmd;   // (Icarus: an automatic task's output into data[k] is lost)
		end
	endtask

	integer i, b, n, idx;
	reg [7:0] chk;

	initial begin
		if ($vcard_open() != 0) begin $display("FAIL can't reach vcardd"); $finish; end
		repeat (10) @(negedge clk); reset = 0; repeat (10) @(negedge clk);

		begin_cmd(16'h80); w(0, r); end_cmd;
		check("menu core extension: version 3", r, 16'd3);
		begin_cmd(16'h81); w(16'h0001, r); end_cmd;
		#(2_000_000);

		// 1. the card as a plain PS1 card, through the existing sector read
		min_reset;
		ps1_read(0);
		check_half("PS1 read at a PS1's 250 kHz", 2000.0);
		check("PS1 read of sector 0: OK", rst, 16'h4000);
		check("sector 0 formatted (MC)", data[0], 16'h434D);

		// the same sector as a raw frame: 81 52 00 MSB LSB, then 134 x 00
		start_frame(8'h52); zeros(138); raw_frame(0, fl);
		check("raw sector read: 140 bytes", rst[7:0], 140);
		check("raw sector read: MC", {fr[10], fr[11]}, 16'h4D43);
		check("raw sector read: end 47", fr[139], 8'h47);

		if ($test$plusargs("stock")) begin
			// stock sd2psXtd: Hello isn't one of its commands, so it stops after 81 60, like a Sony card
			start_frame(8'h60); put("S"); put("M"); put(8'd1); put(8'd1); put(8'd0); zeros(16); raw_frame(0, fl);
			check("stock card: Hello stops at 2", rst[7:0], 2);
			#(60_000_000);   // a refused command can cost the next frame within ~20 ms (08)
			start_frame(8'h61); put_str("SNES", 4); put_str("Bus Test", 8); zeros(2); raw_frame(0, fl);
			check("stock card: Select stops at 2", rst[7:0], 2);
			#(60_000_000);
		end else begin
			// 2. Hello
			start_frame(8'h60); put("S"); put("M"); put(8'd1); put(8'd1); put(8'd0); zeros(16); raw_frame(0, fl);
			check("Hello: whole frame answered", rst[7:0], 23);
			check("Hello: 'S' 'M'", {fr[7], fr[8]}, 16'h534D);
			check("Hello: card version 2", fr[9], 2);

			// 3. Core: SNES is running (card 1); Select SNES / Bus Test on its current card
			start_frame(8'h69); put_str("SNES", 4); put(8'h01); zeros(2); raw_frame(0, fl);
			check("Core: answered", rst[7:0], fl);
			check("Core: 47, card 1", {fr[fl - 2], fr[fl - 1]}, 16'h4701);
			min_reset;
			start_frame(8'h61); put_str("SNES", 4); put_str("Bus Test", 8); put(8'h00); zeros(3); raw_frame(0, fl);
			check_half("Super MemCard frame at 1 MHz", 500.0);
			check("Select: answered", rst[7:0], fl);
			check("Select: 47, no files yet, card 1", {fr[fl - 3], fr[fl - 2], fr[fl - 1]}, 24'h470001);

			// 4. Create save.sav, 256 bytes
			start_frame(8'h64); put(0); put(0); put(8'h01); put(0); put_str("save.sav", 8); zeros(2); raw_frame(0, fl);
			check("Create: 47", fr[fl - 2], 8'h47);
			idx = fr[fl - 1];

			// 5. Write blocks 0 and 1
			for (b = 0; b < 2; b = b + 1) begin
				start_frame(8'h63); put(idx[7:0]); put(0); put(b[7:0]);
				chk = 8'h00 ^ b[7:0];
				for (i = 0; i < 128; i = i + 1) begin put(8'(i * 7 + b * 31)); chk = chk ^ 8'(i * 7 + b * 31); end
				put(chk); zeros(1); raw_frame(0, fl);
				check("Write: whole frame answered", rst[7:0], fl);
				check("Write: 47", fr[fl - 1], 8'h47);
			end

			// 6. Commit generation 1
			start_frame(8'h65); put(idx[7:0]); put(0); put(0); put(0); put(1); zeros(4); zeros(1); raw_frame(0, fl);   // gen 1, time 0
			check("Commit: 47", fr[fl - 1], 8'h47);

			// 7. Read both blocks back
			for (b = 0; b < 2; b = b + 1) begin
				start_frame(8'h62); put(idx[7:0]); put(0); put(b[7:0]); zeros(130); raw_frame(0, fl);
				chk = 8'h00 ^ b[7:0];
				n = 0;
				for (i = 0; i < 128; i = i + 1) begin
					if (fr[5 + i] !== 8'(i * 7 + b * 31)) n = n + 1;
					chk = chk ^ fr[5 + i];
				end
				check("Read: whole frame answered", rst[7:0], fl);
				check("Read: data as written", n, 0);
				check("Read: checksum", fr[5 + 128], chk);
				check("Read: 47", fr[5 + 129], 8'h47);
			end

		end

		// 8. the PS1 card is untouched by all of it
		ps1_read(0);
		check("PS1 read after Super MemCard: OK", rst, 16'h4000);
		check("sector 0 still MC", data[0], 16'h434D);

		// 9. an empty port 2
		start_frame(8'h60); put("S"); put("M"); put(8'd1); put(8'd1); put(8'd0); zeros(16); raw_frame(1, fl);
		check("port 2 empty: nothing answered", rst[7:0], 0);

		$display("card side: %0d bytes, %0d ACKs", card.bytes, card.acks);
		if (errors) $display("%0d FAILED", errors); else $display("ALL PASSED");
		$finish;
	end

	initial begin #(2_000_000_000); $display("TIMEOUT"); $finish; end
endmodule
