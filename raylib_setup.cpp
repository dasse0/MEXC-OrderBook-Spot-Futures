//
// Created by exetrading on 2024-12-31.
//


#include <raylib.h>
#include <iostream>
#include "Programs/OrderBook_MEXC_Spot.h"
#include <vector>
#include <thread>

#include "raylib_setup.h"

Font g_font;  // Definition of the global variable
int raylib_start() {
    const int screenWidth = 500;
    const int screenHeight = 640;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(screenWidth, screenHeight, "MEXC OrderBook Futures");

    // Load custom font
    g_font = LoadFontEx("./resources/Kanit/Kanit-Regular.ttf", 64, nullptr, 0);
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);  // Enable font smoothing

    SetTargetFPS(60);
    SetWindowMinSize(320, 240);

    std::thread ws_thread(MEXC_Connection);
    ws_thread.detach();

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(BLACK);
        RL_MEXC_Orderbook_Spot();
        EndDrawing();
    }

    UnloadFont(g_font);
    CloseWindow();
    return 0;
}
