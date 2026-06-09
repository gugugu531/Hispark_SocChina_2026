# control — 场景自适应曝光校正判决（SDK-free，主机可单元测试）
add_board_module(control)
target_link_libraries(mod_control PUBLIC board_common)

add_board_test(control DEPS mod_control)
