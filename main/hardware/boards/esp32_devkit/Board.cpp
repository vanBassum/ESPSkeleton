#include "Board.h"
#include "esp_log.h"

Board::Board(ServiceProvider &ctx)
    : serviceProvider_(ctx)
{
}

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
