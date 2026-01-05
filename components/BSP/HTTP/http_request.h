#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int heart_rate;
    int breathing_rate;
} health_data_t;

void wifi_init_sta(void);
bool wifi_is_connected(void);
bool wifi_wait_connected(uint32_t timeout_ms);
esp_err_t http_send_health_data(const health_data_t *data);

#endif // HTTP_REQUEST_H
