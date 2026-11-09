// The card end of the PS1 memory card bus, bit by bit, with the virtual card (sd2psXtd's own code) behind it.
// Like the card's PIO programs (src/ps1/ps1_mc_spi.pio): DAT changes on the falling clock edge, the command bit is
// taken on the rising edge, least significant bit first; after a byte the card ACKs if it queued the next reply.
// Copyright (C) 2026 f3bandit.
`timescale 1ns/1ns
module vcard_bus
#(
	parameter ACK_NS = 3000   // the PIO pulls ACK a few us after the byte
)
(
	input        sel_n,
	input        sclk,
	input        cmd,
	output reg   dat,
	output reg   ack_n
);
	reg  [7:0] in_sh, out_sh;
	integer    bitn, r;
	integer    bytes = 0, acks = 0;

	initial begin dat = 1; ack_n = 1; out_sh = 8'hFF; bitn = 0; end

	always @(negedge sel_n) begin $vcard_sel(1); bitn = 0; out_sh = 8'hFF; end
	always @(posedge sel_n) begin $vcard_sel(0); dat = 1; end

	always @(negedge sclk) if (!sel_n) dat = out_sh[bitn];

	always @(posedge sclk) if (!sel_n) begin
		in_sh = {cmd, in_sh[7:1]};
		bitn  = bitn + 1;
		if (bitn == 8) begin
			bitn  = 0;
			r     = $vcard_byte(in_sh);
			bytes = bytes + 1;
			out_sh = r[7:0];
			if (r[8]) begin
				acks = acks + 1;
				#(ACK_NS) ack_n = 0;
				#(2000) ack_n = 1;
			end
		end
	end
endmodule
