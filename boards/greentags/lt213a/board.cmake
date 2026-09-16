set(OPENOCD_NRF5_INTERFACE "cmsis-dap")
board_runner_args(openocd --cmd-pre-init "adapter speed 10000")
include(${ZEPHYR_BASE}/boards/common/openocd-nrf5.board.cmake)

board_runner_args(jlink "--device=nRF51822_xxAB" "--speed=1000")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
