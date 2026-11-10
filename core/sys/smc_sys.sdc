# Super MemCard (smc_sys.sv): the 64 us tick from the 50 MHz reference is synchronised into clk_sys
set_false_path -to [get_keepers {*smc_sys|tick_s[0]}]
