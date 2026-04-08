savedcmd_mz0380.mod := printf '%s\n'   mz0380-cards.o mz0380-core.o | awk '!x[$$0]++ { print("./"$$0) }' > mz0380.mod
