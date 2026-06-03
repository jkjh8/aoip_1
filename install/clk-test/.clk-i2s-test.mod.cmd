savedcmd_clk-i2s-test.mod := printf '%s\n'   clk-i2s-test.o | awk '!x[$$0]++ { print("./"$$0) }' > clk-i2s-test.mod
