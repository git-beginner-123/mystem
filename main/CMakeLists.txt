idf_component_register(
    SRCS
        "app_main.c"

        "core/app.c"
        "core/app_state.c"
        "core/app_events.c"

        "ui/ui_lcd.c"
        "ui/ui_console.c"
        "input/input_uart_frame.c"

        "display/st7735.c"
        "display/font5x7.c"
        "display/font8x16.c"
        "experiments/experiments_registry.c"

        "experiments/exp_gpio.c"
        "experiments/exp_pwm.c"
        "experiments/exp_adc.c"
        "experiments/exp_uart.c"
        "experiments/exp_tof.c"
        "experiments/exp_mic.c"
        "experiments/exp_maze.c"
        "experiments/exp_speaker.c"
        "experiments/exp_ble.c"
        "experiments/exp_wifi_ap.c"
        "experiments/exp_wifi_sta.c"
        "experiments/exp_semaforo.c"
        "experiments/exp_lcd_color.c"
        "input/uart1_router.c"
        "input/drv_input_gpio_keys.c"
        "net/remote_web.c"
        "net/market_yahoo.c"

    INCLUDE_DIRS
        "."
        "core"
        "ui"
        "input"
        "display"
        "experiments"

    EMBED_FILES
        "core/hola_es.pcm"

    REQUIRES
        esp_driver_uart
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2s
        driver
        freertos
        esp_system
        esp_common
        log
        esp_http_server
        esp_http_client
        esp_netif
        esp_event
        esp_wifi
        bt
        nvs_flash
        mbedtls
        comm_wifi
        comm_ble
        json
)
