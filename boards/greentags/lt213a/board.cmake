board_runner_args(jlink "--device=nRF51822_xxAB" "--speed=1000")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd-nrf5.board.cmake)
