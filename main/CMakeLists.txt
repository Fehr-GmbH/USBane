idf_component_register(
    SRCS "usbane.c" "main.c" "dwc2_backend.c" "soft_backend.c" "web_interface.c" "wifi_ap.c" "chain_engine.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server nvs_flash cjson esp_event driver
    PRIV_REQUIRES usb esp_http_client esp_driver_gpio
    EMBED_FILES "html/index.html" "html/app.js" "html/logo.svg" "html/applogo.png" "html/apptext.svg" "html/favicon.ico" "html/api.html" "html/openapi.json"
)
