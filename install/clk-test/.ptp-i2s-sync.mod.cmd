savedcmd_ptp-i2s-sync.mod := printf '%s\n'   ptp-i2s-sync.o | awk '!x[$$0]++ { print("./"$$0) }' > ptp-i2s-sync.mod
