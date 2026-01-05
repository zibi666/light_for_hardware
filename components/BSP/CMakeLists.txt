set(src_dirs
            UART
            Protocol
            HTTP)

set(include_dirs
            UART
            Protocol
            HTTP)

set(requires
            driver
            esp_wifi
            esp_event
            esp_netif
            esp_http_client
            json)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)