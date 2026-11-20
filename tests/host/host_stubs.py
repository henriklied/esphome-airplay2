"""ESP-IDF and lwIP header stubs for compiling component sources on the host."""

from pathlib import Path

STUBS = {
    "esp_err.h": "#pragma once\nusing esp_err_t = int;\n"
    "constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_NO_MEM = 2;\n",
    "freertos/FreeRTOS.h": "#pragma once\n#include <cstdint>\n"
    "using TickType_t = uint32_t;\nconstexpr int pdTRUE = 1;\n"
    "#define pdMS_TO_TICKS(x) (x)\n",
    "freertos/portmacro.h": "#pragma once\nusing portMUX_TYPE = int;\n"
    "#define portMUX_INITIALIZER_UNLOCKED 0\n"
    "#define portENTER_CRITICAL(x) ((void)(x))\n"
    "#define portEXIT_CRITICAL(x) ((void)(x))\n",
    "freertos/semphr.h": "#pragma once\n#include <cstdlib>\n"
    "using SemaphoreHandle_t = void *;\n"
    "inline void *xSemaphoreCreateBinary() { return std::malloc(1); }\n"
    "inline void vSemaphoreDelete(void *x) { std::free(x); }\n"
    "inline int xSemaphoreGive(void *) { return 1; }\n"
    "inline int xSemaphoreTake(void *, unsigned) { return 1; }\n",
    "freertos/task.h": "#pragma once\ninline void vTaskDelay(unsigned) {}\n",
    "esp_timer.h": "#pragma once\n#include <cstdint>\n"
    "inline int64_t esp_timer_get_time() { return 0; }\n",
    "lwip/api.h": "#pragma once\n#include <cstdint>\n"
    "using err_t = int;\nconstexpr int ERR_OK = 0, ERR_MEM = -1;\n"
    "struct sys_mbox_t { unsigned used; unsigned capacity; };\n"
    "constexpr int NETCONN_UDP = 1, NETCONN_TCP = 2;\n"
    "struct netconn { int type; sys_mbox_t recvmbox; };\n"
    "#define netconn_type(x) ((x)->type)\n#define NETCONNTYPE_GROUP(x) (x)\n",
    "lwip/priv/sockets_priv.h": "#pragma once\n#include \"lwip/api.h\"\n"
    "struct lwip_sock { netconn *conn; };\n"
    "extern \"C\" lwip_sock *lwip_socket_dbg_get_socket(int);\n",
    "lwip/tcpip.h": "#pragma once\n#include \"lwip/api.h\"\n"
    "using tcpip_callback_fn = void (*)(void *);\n"
    "extern \"C\" err_t tcpip_callback_wait(tcpip_callback_fn, void *);\n",
}


def write_stubs(directory: Path) -> None:
    """Write every stub header under `directory`."""
    for relative, content in STUBS.items():
        path = directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
