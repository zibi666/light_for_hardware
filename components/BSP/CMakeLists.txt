set(src_dirs
            UART
            Protocol
            HTTP
            SleepAnalysis
            Audio
            Input
            App
            RTC
            AlarmMusic)

set(include_dirs
            UART
            Protocol
            HTTP
            SleepAnalysis
            Audio
            Input
            App
            RTC
            AlarmMusic)

set(requires
            driver
            esp_wifi
            esp_event
            esp_netif
            esp_http_client
            json
            fatfs
            sdmmc
            lwip)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)