idf_component_register(
    SRCS "main.c" "usb_malformed.c" "web_interface.c" "wifi_ap.c"
    INCLUDE_DIRS "."
    EMBED_FILES "html/index.html" "html/app.js" "html/logo.svg" "html/applogo.png" "html/apptext.svg" "html/favicon.ico" "html/api.html" "html/openapi.json"
)

