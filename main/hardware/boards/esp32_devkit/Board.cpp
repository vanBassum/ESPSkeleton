#include "Board.h"
#include "esp_log.h"

void Board::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    led_.Init();

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}
