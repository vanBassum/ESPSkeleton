#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "Board.h"
#include "StruxContext.h"
#include "ApplicationContext.h"

static const char* TAG = "main";

// Three layers, bottom to top, and the dependencies run one way only:
//
//   Board            the hardware. Knows nothing above it.
//   StruxContext     the framework. Knows the board? No — see StruxServices.h.
//   ApplicationContext   this product. Knows both.
//
// Each layer owns its instances (its context) and exposes what the layer above may reach
// for (its provider: StruxServices, AppServices — the board's is its own class surface,
// checked at compile time rather than through a vtable).
//
// This file is deliberately four calls long. The ORDER WITHIN each layer lives in that
// layer's context, so a fork pulling a new Strux manager gets its position along with it
// instead of having to be told.
Board board;
StruxContext strux;
ApplicationContext application{ board, strux };

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting up...");

    board.Init();         // hardware first: it depends on nothing
    strux.Init();         // then the framework the application registers into
    application.Init();   // then the product

    // Mark firmware as valid so the bootloader doesn't roll back on next reboot
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "All layers initialized, firmware confirmed valid");
}
