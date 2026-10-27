set(TUSB "${CMAKE_CURRENT_LIST_DIR}/../../vendor/tinyusb/src")
if(NOT EXISTS "${TUSB}/tusb.c")
    message(FATAL_ERROR "请先执行 git submodule update --init --recursive")
endif()
# 锁定上游，并给异步取消及 USB 所有者任务完成增加小范围适配。
file(READ "${TUSB}/class/msc/msc_device.c" MSC)
set(RESET_A "void mscd_reset(uint8_t rhport) {")
set(RESET_B "static void proc_bot_reset(mscd_interface_t* p_msc) {")
set(DEFER "usbd_defer_func(proc_async_io_done, (void *) (intptr_t) bytes_io, in_isr);")
foreach(ANCHOR IN ITEMS "${RESET_A}" "${RESET_B}" "${DEFER}")
    string(FIND "${MSC}" "${ANCHOR}" POS)
    if(POS EQUAL -1)
        message(FATAL_ERROR "TinyUSB MSC 源码与适配不匹配；不要直接升级子模块")
    endif()
endforeach()
string(REPLACE "${RESET_A}" "extern void w2b_msc_cancel(void);\n${RESET_A}\n  w2b_msc_cancel();" MSC "${MSC}")
string(REPLACE "${RESET_B}" "${RESET_B}\n  w2b_msc_cancel();\n  p_msc->pending_io = false;" MSC "${MSC}")
# 本工程所有 TinyUSB API 只由 USB 任务调用，禁止 ISR/网络任务完成 MSC。
# 同任务立即完成，避免排队完成事件跨过总线/BOT 重置而误完成下一次读取。
string(REPLACE "${DEFER}" "TU_VERIFY(!in_isr);\n  proc_async_io_done((void *) (intptr_t) bytes_io);" MSC "${MSC}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/msc_device_patched.c" "${MSC}")
idf_component_register(
    SRCS "${TUSB}/tusb.c" "${TUSB}/common/tusb_fifo.c"
         "${TUSB}/device/usbd.c" "${TUSB}/class/hid/hid_device.c"
         "${CMAKE_CURRENT_BINARY_DIR}/msc_device_patched.c"
         "${TUSB}/portable/synopsys/dwc2/dcd_dwc2.c"
         "${TUSB}/portable/synopsys/dwc2/dwc2_common.c"
    INCLUDE_DIRS "${TUSB}" "${TUSB}/class/msc" "../../usb_config"
    REQUIRES freertos esp_hw_support esp_system soc hal usb esp_mm)
