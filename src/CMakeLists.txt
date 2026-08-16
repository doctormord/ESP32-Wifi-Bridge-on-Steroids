# Hauptkomponente. Die REQUIRES-Liste muss alle IDF-Komponenten nennen, deren
# Header wir einbinden - anders als bei Arduino-as-component gibt es hier
# keine Sammel-Abhaengigkeit, die stillschweigend alles mitzieht.
idf_component_register(
    SRCS
        "main.cpp"
        "bridge.cpp"
        "config.cpp"
        "web.cpp"
        "telemetry.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_eth              # Ethernet-MAC/PHY
        esp_wifi             # WLAN inkl. esp_wifi_internal_*
        esp_netif            # Management-Netif, esp_netif_receive
        esp_event
        esp_timer            # millis() in compat.h
        nvs_flash            # Konfigurationsspeicher
        esp_http_server      # Portal und Status-API
        mqtt                 # Telemetrie
        app_update           # OTA (esp_ota_*)
        driver               # GPIO
        esp_hw_support
        lwip                 # ping_sock.h - Standorterkennung per Gateway-Ping
)
