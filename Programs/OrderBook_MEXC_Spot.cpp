#include "OrderBook_MEXC_Spot.h"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <raylib.h>
#include <mutex>
#include <memory>
#include <chrono>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

struct PriceTrend {
    float shortTermMA;  // Short-term moving average
    float longTermMA;   // Long-term moving average
    float momentum;     // Price momentum indicator
    std::vector<float> recentPrices;  // Circular buffer for recent prices
    size_t priceIndex = 0;
    static constexpr size_t PRICE_HISTORY = 100;
};

struct OrderBookMetrics {
    float spreadAmount;
    float spreadPercentage;
    float midPrice;
    float liquidityImbalance;
    PriceTrend trend;
    std::chrono::system_clock::time_point lastUpdate;
};

struct MarketDepthMetrics {
    float cumulativeBidVolume;
    float cumulativeAskVolume;
    float depthImbalanceRatio;
    std::vector<float> bidDepthCurve;
    std::vector<float> askDepthCurve;
};

struct OrderFlowMetrics {
    float buyVolume;
    float sellVolume;
    float netFlow;
    float imbalanceRatio;
    std::vector<float> flowHistory;
    static constexpr size_t FLOW_HISTORY_SIZE = 100;
};

struct OrderEntry {
    std::string priceStr;  // Original string price
    std::string volumeStr; // Original string volume
    float price;          // Cached float price
    float volume;         // Cached float volume
};

std::vector<OrderEntry> g_bids;
std::vector<OrderEntry> g_asks;
std::mutex g_mutex;

char g_baseInput[32] = "ETH";
char g_quoteInput[32] = "USDT";
bool g_shouldRefresh = false;

std::unique_ptr<net::io_context> g_ioc;
std::unique_ptr<ssl::context> g_ctx;

char g_formatBuffer[64];
inline const char* formatNumber(float value) {
    if (value >= 1000000) {
        snprintf(g_formatBuffer, sizeof(g_formatBuffer), "%.2fM", value / 1000000);
    } else if (value >= 1000) {
        snprintf(g_formatBuffer, sizeof(g_formatBuffer), "%.2fK", value / 1000);
    } else {
        snprintf(g_formatBuffer, sizeof(g_formatBuffer), "%.2f", value);
    }
    return g_formatBuffer;
}

void DrawOrderBookHeatmap(
    const std::vector<OrderEntry>& orders,
    int startX, int width, int topbarHeight, int height,
    const OrderBookMetrics& metrics, bool isBid) 
{
    if (orders.empty()) return;
    
    // Find max volume for scaling - use cached float values
    float maxVolume = 0.0f;
    for (const auto& order : orders) {
        maxVolume = std::max(maxVolume, order.volume);
    }
    
    // Updated heatmap colors with better transparency
    const Color heatmapColors[] = {
        isBid ? Color{0, 0, 128, 25} : Color{128, 0, 0, 25},    // High volume
        isBid ? Color{0, 0, 96, 20} : Color{96, 0, 0, 20},      // Medium volume
        isBid ? Color{0, 0, 64, 15} : Color{64, 0, 0, 15}       // Low volume
    };
    
    const int ROW_HEIGHT = 30;
    
    for (size_t i = 0; i < orders.size(); i++) {
        float volume = orders[i].volume;
        float y = topbarHeight + (i * ROW_HEIGHT);
        
        // Calculate intensity based on volume
        float intensity = volume / maxVolume;
        
        // Select color based on intensity
        int colorIndex = std::min(
            static_cast<int>(intensity * (sizeof(heatmapColors)/sizeof(heatmapColors[0]) - 1)),
            static_cast<int>(sizeof(heatmapColors)/sizeof(heatmapColors[0]) - 1)
        );
        
        // Draw heatmap overlay
        DrawRectangle(startX, y, width, ROW_HEIGHT, heatmapColors[colorIndex]);
    }
}

void updatePriceTrend(PriceTrend& trend, float currentPrice) {
    if (trend.recentPrices.size() < PriceTrend::PRICE_HISTORY) {
        trend.recentPrices.resize(PriceTrend::PRICE_HISTORY, currentPrice);
    }
    
    trend.recentPrices[trend.priceIndex] = currentPrice;
    trend.priceIndex = (trend.priceIndex + 1) % PriceTrend::PRICE_HISTORY;
    
    // Calculate moving averages
    float shortTermSum = 0.0f;
    float longTermSum = 0.0f;
    const size_t shortTermPeriod = 10;
    const size_t longTermPeriod = 30;
    
    for (size_t i = 0; i < PriceTrend::PRICE_HISTORY; i++) {
        if (i < shortTermPeriod) shortTermSum += trend.recentPrices[i];
        if (i < longTermPeriod) longTermSum += trend.recentPrices[i];
    }
    
    trend.shortTermMA = shortTermSum / shortTermPeriod;
    trend.longTermMA = longTermSum / longTermPeriod;
    
    // Calculate momentum
    float recentChange = trend.recentPrices[trend.priceIndex] - 
                        trend.recentPrices[(trend.priceIndex + PriceTrend::PRICE_HISTORY - 10) % PriceTrend::PRICE_HISTORY];
    trend.momentum = recentChange / trend.recentPrices[trend.priceIndex];
}

float calculateTWAP(const std::vector<float>& prices, 
                   const std::vector<std::chrono::system_clock::time_point>& timestamps) {
    if (prices.empty()) return 0.0f;
    
    float twap = 0.0f;
    float totalWeight = 0.0f;
    
    for (size_t i = 1; i < prices.size(); i++) {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamps[i] - timestamps[i-1]).count();
        float weight = static_cast<float>(duration);
        twap += prices[i] * weight;
        totalWeight += weight;
    }
    
    return totalWeight > 0 ? twap / totalWeight : prices.back();
}

OrderBookMetrics calculateOrderBookMetrics(
    const std::vector<OrderEntry>& bids,
    const std::vector<OrderEntry>& asks,
    const PriceTrend& currentTrend) 
{
    OrderBookMetrics metrics = {0};
    metrics.lastUpdate = std::chrono::system_clock::now();
    
    if (bids.empty() || asks.empty()) return metrics;
    
    // Use cached float values instead of string conversions
    float bestBid = bids[0].price;
    float bestAsk = asks[0].price;
    
    metrics.spreadAmount = bestAsk - bestBid;
    metrics.midPrice = (bestAsk + bestBid) / 2.0f;
    metrics.spreadPercentage = (metrics.spreadAmount / metrics.midPrice) * 100.0f;
    
    // Calculate liquidity imbalance
    float bidLiquidity = 0.0f;
    float askLiquidity = 0.0f;
    
    for (const auto& bid : bids) {
        bidLiquidity += bid.volume;
    }
    
    for (const auto& ask : asks) {
        askLiquidity += ask.volume;
    }
    
    metrics.liquidityImbalance = (bidLiquidity - askLiquidity) / (bidLiquidity + askLiquidity);
    metrics.trend = currentTrend;
    
    return metrics;
}

// Function to render and handle buttons
void RL_MEXC_Orderbook_Spot_Topbar() {
    const Color HEADER_BG = {20, 20, 30, 255};
    const Color INPUT_BG = {33, 33, 33, 255};
    const Color INPUT_ACTIVE_BG = {45, 45, 45, 255};
    const Color BUTTON_COLOR = {40, 80, 120, 255};
    const Color BUTTON_HOVER_COLOR = {50, 100, 150, 255};
    
    // Draw the background
    DrawRectangle(0, 0, GetScreenWidth(), 40, HEADER_BG);
    
    // Input dimensions
    const int INPUT_WIDTH = 100;
    const int INPUT_HEIGHT = 30;
    const int INPUT_PADDING = 10;
    const int START_X = 20;
    
    // Define input rectangles
    Rectangle baseRect = {START_X, 5, INPUT_WIDTH, INPUT_HEIGHT};
    Rectangle quoteRect = {START_X + INPUT_WIDTH + INPUT_PADDING, 5, INPUT_WIDTH, INPUT_HEIGHT};
    Rectangle enterRect = {START_X + (INPUT_WIDTH + INPUT_PADDING) * 2, 5, INPUT_WIDTH, INPUT_HEIGHT};
    
    static bool baseActive = false;
    static bool quoteActive = false;
    
    // Handle input focus
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        Vector2 mousePos = GetMousePosition();
        baseActive = CheckCollisionPointRec(mousePos, baseRect);
        quoteActive = CheckCollisionPointRec(mousePos, quoteRect);
    }
    
    // Draw input boxes
    DrawRectangleRec(baseRect, baseActive ? INPUT_ACTIVE_BG : INPUT_BG);
    DrawRectangleRec(quoteRect, quoteActive ? INPUT_ACTIVE_BG : INPUT_BG);
    
    // Handle text input
    if (baseActive) {
        int key = GetCharPressed();
        while (key > 0) {
            if ((key >= 32) && (key <= 125) && (strlen(g_baseInput) < sizeof(g_baseInput) - 1)) {
                g_baseInput[strlen(g_baseInput)] = (char)key;
                g_baseInput[strlen(g_baseInput)] = '\0';
            }
            key = GetCharPressed();
        }
        
        if (IsKeyPressed(KEY_BACKSPACE) && strlen(g_baseInput) > 0) {
            g_baseInput[strlen(g_baseInput) - 1] = '\0';
        }
    }
    
    if (quoteActive) {
        int key = GetCharPressed();
        while (key > 0) {
            if ((key >= 32) && (key <= 125) && (strlen(g_quoteInput) < sizeof(g_quoteInput) - 1)) {
                g_quoteInput[strlen(g_quoteInput)] = (char)key;
                g_quoteInput[strlen(g_quoteInput)] = '\0';
            }
            key = GetCharPressed();
        }
        
        if (IsKeyPressed(KEY_BACKSPACE) && strlen(g_quoteInput) > 0) {
            g_quoteInput[strlen(g_quoteInput) - 1] = '\0';
        }
    }
    
    // Draw input text
    DrawTextEx(g_font, g_baseInput, {baseRect.x + 5, baseRect.y + 5}, 20, 1, WHITE);
    DrawTextEx(g_font, g_quoteInput, {quoteRect.x + 5, quoteRect.y + 5}, 20, 1, WHITE);
    
    // Draw and handle enter button
    Vector2 mousePos = GetMousePosition();
    bool isHovered = CheckCollisionPointRec(mousePos, enterRect);
    
    DrawRectangleRec(enterRect, isHovered ? BUTTON_HOVER_COLOR : BUTTON_COLOR);
    DrawTextEx(g_font, "Enter", {enterRect.x + 25, enterRect.y + 5}, 20, 1, WHITE);
    
    if (isHovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        g_shouldRefresh = true;
    }
}

// Add this function to draw market statistics
void DrawMarketStats(const OrderBookMetrics& metrics, int topbarHeight) {
    const int STATS_HEIGHT = 60;
    const Color STATS_BG = {15, 15, 25, 255};
    const int PADDING = 10;
    
    // Draw stats background
    DrawRectangle(0, topbarHeight, GetScreenWidth(), STATS_HEIGHT, STATS_BG);
    
    // Format statistics (removed TWAP and VWAP)
    char stats[2][64];
    snprintf(stats[0], sizeof(stats[0]), "Spread: %.2f (%.2f%%)", 
             metrics.spreadAmount, metrics.spreadPercentage);
    snprintf(stats[1], sizeof(stats[1]), "B/A Ratio: %.2f%%",
             metrics.liquidityImbalance * 100.0f);
    
    // Draw statistics with improved layout
    const int STAT_WIDTH = GetScreenWidth() / 2 + 2 ;  // Adjusted for fewer stats
    for (int i = 0; i < 2; i++) {
        Color textColor = WHITE;
        if (i == 1) {  // Color imbalance based on value
            textColor = metrics.liquidityImbalance > 0 ? GREEN : RED;
        }
        
        Vector2 pos = {
            (float)(i * STAT_WIDTH + PADDING),
            (float)(topbarHeight + (STATS_HEIGHT - 20) / 2)
        };
        DrawTextEx(g_font, stats[i], pos, 20, 1, textColor);
    }
}

void DrawOrderbookRowsWithThresholds(
    const std::vector<OrderEntry>& orders,
    Font& font, int startX, int width, int topbarHeight, int height,
    const std::vector<Color>& colors, bool isBid, float midPrice, 
    const OrderBookMetrics& metrics)
{
    if (orders.empty()) return;
    
    const int ROW_HEIGHT = 18;  // Slightly reduced height
    const int PRICE_WIDTH = width * 0.55;  // Adjusted column proportions
    const int VOLUME_WIDTH = width * 0.45;
    const int TEXT_SIZE = 26;  // Slightly smaller text
    const int PADDING = 8;

    // Updated header style
    const Color HEADER_BG = {25, 28, 36, 255};  // Darker, more professional header
    DrawRectangle(startX, topbarHeight, width, ROW_HEIGHT, HEADER_BG);
    
    // Add subtle gradient to header
    Color gradientColor = {35, 38, 46, 100};
    DrawRectangleGradientH(startX, topbarHeight, width, ROW_HEIGHT, HEADER_BG, gradientColor);

    // Draw headers with improved positioning
    Vector2 priceHeaderPos = {(float)startX + PADDING, (float)topbarHeight + (ROW_HEIGHT - TEXT_SIZE) / 2};
    Vector2 volumeHeaderPos = {(float)startX + PRICE_WIDTH + PADDING, (float)topbarHeight + (ROW_HEIGHT - TEXT_SIZE) / 2};
    DrawTextEx(font, "Price", priceHeaderPos, TEXT_SIZE, 1, {180, 180, 180, 255});
    DrawTextEx(font, "Amount", volumeHeaderPos, TEXT_SIZE, 1, {180, 180, 180, 255});

    // Draw separator line
    DrawLine(startX + PRICE_WIDTH, topbarHeight,
             startX + PRICE_WIDTH, topbarHeight + height,
             {40, 40, 50, 255});

    // Find max volume for color scaling
    float maxVolume = 0.0f;
    for (const auto& order : orders) {
        maxVolume = std::max(maxVolume, order.volume);
    }

    // Draw rows with improved styling
    for (size_t i = 0; i < orders.size(); i++) {
        float price = orders[i].price;
        float volume = orders[i].volume;
        float y = topbarHeight + ((i + 1) * ROW_HEIGHT);

        // Calculate color index with smoother transition
        float volumeRatio = volume / (maxVolume * 1.2f);  // Adjusted scaling
        int colorIndex = std::min(
            static_cast<int>(volumeRatio * (colors.size() - 1)),
            static_cast<int>(colors.size() - 1)
        );

        // Draw row background with subtle gradient
        Color baseColor = colors[colorIndex];
        Color gradientEnd = {
            (unsigned char)(baseColor.r * 0.9),
            (unsigned char)(baseColor.g * 0.9),
            (unsigned char)(baseColor.b * 0.9),
            255
        };
        DrawRectangleGradientH(startX, y, width, ROW_HEIGHT, baseColor, gradientEnd);

        // Format price and volume with improved number formatting
        char priceStr[32], volumeStr[32];
        snprintf(priceStr, sizeof(priceStr), "%.2f", price);
        
        // Format volume with K/M suffixes
        if (volume >= 1000000) {
            snprintf(volumeStr, sizeof(volumeStr), "%.2fM", volume / 1000000);
        } else if (volume >= 1000) {
            snprintf(volumeStr, sizeof(volumeStr), "%.2fK", volume / 1000);
        } else {
            snprintf(volumeStr, sizeof(volumeStr), "%.2f", volume);
        }

        // Draw text with improved positioning and colors
        Vector2 pricePos = {(float)startX + PADDING, y + (ROW_HEIGHT - TEXT_SIZE) / 2};
        Vector2 volumePos = {(float)startX + PRICE_WIDTH + PADDING, y + (ROW_HEIGHT - TEXT_SIZE) / 2};
        
        Color textColor = {230, 230, 230, 255};  // Brighter text for better contrast
        DrawTextEx(font, priceStr, pricePos, TEXT_SIZE, 1, textColor);
        DrawTextEx(font, volumeStr, volumePos, TEXT_SIZE, 1, textColor);
    }

    // Add subtle separator between price and volume columns
    DrawLine(startX + PRICE_WIDTH, topbarHeight,
             startX + PRICE_WIDTH, topbarHeight + height,
             {45, 48, 56, 255});  // Darker separator color
}

// Add this function to calculate market depth metrics
MarketDepthMetrics calculateMarketDepth(
    const std::vector<OrderEntry>& bids,
    const std::vector<OrderEntry>& asks)
{
    MarketDepthMetrics metrics;
    metrics.cumulativeBidVolume = 0;
    metrics.cumulativeAskVolume = 0;

    // Calculate cumulative volumes and depth curves
    for (const auto& bid : bids) {
        float volume = bid.volume;
        metrics.cumulativeBidVolume += volume;
        metrics.bidDepthCurve.push_back(metrics.cumulativeBidVolume);
    }

    for (const auto& ask : asks) {
        float volume = ask.volume;
        metrics.cumulativeAskVolume += volume;
        metrics.askDepthCurve.push_back(metrics.cumulativeAskVolume);
    }

    float totalDepth = metrics.cumulativeBidVolume + metrics.cumulativeAskVolume;
    metrics.depthImbalanceRatio = totalDepth > 0 ?
        (metrics.cumulativeBidVolume - metrics.cumulativeAskVolume) / totalDepth : 0;

    return metrics;
}

// Add this function to draw the market depth curve
void DrawMarketDepthCurve(const MarketDepthMetrics& metrics, int x, int y, int width, int height) {
    const Color BID_COLOR = {0, 150, 255, 255};      // Brighter blue
    const Color ASK_COLOR = {255, 95, 95, 255};      // Brighter red
    const Color GRID_COLOR = {40, 45, 60, 100};      // Matching grid color
    const Color AXIS_COLOR = {80, 85, 100, 255};     // Brighter axes
    
    // Draw enhanced grid
    const int GRID_LINES = 6;
    for (int i = 1; i < GRID_LINES; i++) {
        float yPos = y + (height * i) / GRID_LINES;
        float xPos = x + (width * i) / GRID_LINES;
        DrawLineEx({(float)x, yPos}, {(float)(x + width), yPos}, 1, GRID_COLOR);
        DrawLineEx({xPos, (float)y}, {xPos, (float)(y + height)}, 1, GRID_COLOR);
    }
    
    // Draw axes with subtle glow
    DrawLineEx({(float)x, (float)(y + height)}, {(float)(x + width), (float)(y + height)}, 3, {80, 85, 100, 30});
    DrawLineEx({(float)x, (float)y}, {(float)x, (float)(y + height)}, 3, {80, 85, 100, 30});
    DrawLineEx({(float)x, (float)(y + height)}, {(float)(x + width), (float)(y + height)}, 2, AXIS_COLOR);
    DrawLineEx({(float)x, (float)y}, {(float)x, (float)(y + height)}, 2, AXIS_COLOR);
    
    float maxDepth = std::max(metrics.cumulativeBidVolume, metrics.cumulativeAskVolume);
    
    // Draw bid curve with glow effect
    std::vector<Vector2> bidPoints;
    for (size_t i = 0; i < metrics.bidDepthCurve.size(); i++) {
        bidPoints.push_back({
            static_cast<float>(x + i * width / metrics.bidDepthCurve.size()),
            static_cast<float>(y + height - (metrics.bidDepthCurve[i] / maxDepth) * height)
        });
    }
    
    // Draw bid curve with enhanced glow effect
    for (size_t i = 1; i < bidPoints.size(); i++) {
        Vector2 start = bidPoints[i-1];
        Vector2 end = bidPoints[i];
        DrawLineEx(start, end, 4, {0, 150, 255, 20});  // Outer glow
        DrawLineEx(start, end, 2, {0, 150, 255, 40});  // Inner glow
        DrawLineEx(start, end, 1, BID_COLOR);          // Main line
    }
    
    // Draw ask curve with glow effect
    std::vector<Vector2> askPoints;
    for (size_t i = 0; i < metrics.askDepthCurve.size(); i++) {
        askPoints.push_back({
            static_cast<float>(x + i * width / metrics.askDepthCurve.size()),
            static_cast<float>(y + height - (metrics.askDepthCurve[i] / maxDepth) * height)
        });
    }
    
    // Draw ask curve with enhanced glow effect
    for (size_t i = 1; i < askPoints.size(); i++) {
        Vector2 start = askPoints[i-1];
        Vector2 end = askPoints[i];
        DrawLineEx(start, end, 4, {255, 95, 95, 20});  // Outer glow
        DrawLineEx(start, end, 2, {255, 95, 95, 40});  // Inner glow
        DrawLineEx(start, end, 1, ASK_COLOR);          // Main line
    }
    
    // Draw volume labels with improved styling
    char volumeLabel[32];
    for (int i = 0; i <= GRID_LINES; i++) {
        float volume = (maxDepth * (GRID_LINES - i)) / GRID_LINES;
        
        // Format volume with K/M suffixes
        if (volume >= 1000000) {
            snprintf(volumeLabel, sizeof(volumeLabel), "%.1fM", volume / 1000000);
        } else if (volume >= 1000) {
            snprintf(volumeLabel, sizeof(volumeLabel), "%.1fK", volume / 1000);
        } else {
            snprintf(volumeLabel, sizeof(volumeLabel), "%.1f", volume);
        }
        
        // Draw label text with shadow
        Vector2 textPos = {(float)x - 65, (float)(y + (height * i) / GRID_LINES - 10)};
        DrawTextEx(g_font, volumeLabel, {textPos.x + 1, textPos.y + 1}, 16, 1, {0, 0, 0, 128});
        DrawTextEx(g_font, volumeLabel, textPos, 16, 1, {220, 220, 220, 255});
    }
}

void RL_MEXC_Orderbook_Spot() {
    static PriceTrend currentTrend;
    static std::vector<float> recentPrices;
    static std::vector<std::chrono::system_clock::time_point> timestamps;

    std::lock_guard<std::mutex> lock(g_mutex);

    // Constants for layout
    const int TOPBAR_HEIGHT = 40;
    const int STATS_HEIGHT = 60;
    const int CONTENT_START = TOPBAR_HEIGHT + STATS_HEIGHT;
    const int ROW_HEIGHT = 30;
    const int COLUMN_WIDTH = GetScreenWidth() / 2;

    // Font setup
    static Font customFont = {0};
    static bool fontLoaded = false;
    if (!fontLoaded) {
        customFont = LoadFont("../resources/Kanit/Kanit-Regular.ttf");
        SetTextureFilter(customFont.texture, TEXTURE_FILTER_BILINEAR);
        fontLoaded = true;
    }

    // Update price trend
    if (!g_bids.empty() && !g_asks.empty()) {
        float midPrice = (g_bids[0].price + g_asks[0].price) / 2.0f;
        updatePriceTrend(currentTrend, midPrice);

        recentPrices.push_back(midPrice);
        timestamps.push_back(std::chrono::system_clock::now());

        if (recentPrices.size() > 100) {
            recentPrices.erase(recentPrices.begin());
            timestamps.erase(timestamps.begin());
        }
    }

    // Calculate metrics first
    OrderBookMetrics metrics = calculateOrderBookMetrics(g_bids, g_asks, currentTrend);
    MarketDepthMetrics depthMetrics = calculateMarketDepth(g_bids, g_asks);

    // Now start drawing, beginning with the topbar
    RL_MEXC_Orderbook_Spot_Topbar();

    // Draw market statistics below topbar
    DrawMarketStats(metrics, TOPBAR_HEIGHT);

    // Layout adjustments with proper spacing
    const int DEPTH_CHART_HEIGHT = 150;
    const int ORDERBOOK_START = CONTENT_START + DEPTH_CHART_HEIGHT;

    // Draw market depth curve below stats
    DrawMarketDepthCurve(depthMetrics, 0, CONTENT_START,
                        GetScreenWidth(), DEPTH_CHART_HEIGHT);

    // Update the color constants to use pure blue/red gradients
    const std::vector<Color> bidColors = {
        {0, 0, 50, 255},     // Darkest blue
        {0, 0, 90, 255},
        {0, 0, 130, 255},
        {0, 0, 170, 255},
        {0, 0, 210, 255},
        {0, 0, 255, 255}     // Brightest blue
    };

    const std::vector<Color> askColors = {
        {50, 0, 0, 255},     // Darkest red
        {90, 0, 0, 255},
        {130, 0, 0, 255},
        {170, 0, 0, 255},
        {210, 0, 0, 255},
        {255, 0, 0, 255}     // Brightest red
    };

    // Adjust orderbook position
    DrawRectangle(0, ORDERBOOK_START, GetScreenWidth(),
                 GetScreenHeight() - ORDERBOOK_START, BLACK);

    // Update orderbook drawing calls with new vertical offset
    DrawOrderbookRowsWithThresholds(
        g_bids, customFont, 0, COLUMN_WIDTH, ORDERBOOK_START,
        GetScreenHeight() - ORDERBOOK_START, bidColors, true, metrics.midPrice, metrics
    );

    DrawOrderbookRowsWithThresholds(
        g_asks, customFont, COLUMN_WIDTH, COLUMN_WIDTH, ORDERBOOK_START,
        GetScreenHeight() - ORDERBOOK_START, askColors, false, metrics.midPrice, metrics
    );

    // Draw heatmap overlays with adjusted position
    DrawOrderBookHeatmap(g_bids, 0, COLUMN_WIDTH, CONTENT_START,
                        GetScreenHeight() - CONTENT_START, metrics, true);
    DrawOrderBookHeatmap(g_asks, COLUMN_WIDTH, COLUMN_WIDTH, CONTENT_START,
                        GetScreenHeight() - CONTENT_START, metrics, false);
}

// Add this helper function before MEXC_Connection()
websocket::stream<beast::ssl_stream<tcp::socket>>* setupWebSocket() {
    // Create contexts if they don't exist
    if (!g_ioc) {
        g_ioc = std::make_unique<net::io_context>();
    }
    if (!g_ctx) {
        g_ctx = std::make_unique<ssl::context>(ssl::context::tlsv12_client);
        g_ctx->set_verify_mode(ssl::verify_none);
    }

    // Create WebSocket
    auto ws = new websocket::stream<beast::ssl_stream<tcp::socket>>(*g_ioc, *g_ctx);

    try {
        // DNS lookup
        tcp::resolver resolver{*g_ioc};
        auto const results = resolver.resolve("wbs.mexc.com", "443");

        // Connect to IP
        auto ep = net::connect(get_lowest_layer(*ws), results);

        // SSL handshake
        if(!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), "wbs.mexc.com")) {
            throw beast::system_error(
                beast::error_code(
                    static_cast<int>(::ERR_get_error()),
                    net::error::get_ssl_category()
                )
            );
        }

        // Perform SSL handshake
        ws->next_layer().handshake(ssl::stream_base::client);

        // Perform WebSocket handshake
        ws->handshake("wbs.mexc.com", "/ws");

        std::cout << "Successfully connected to MEXC WebSocket" << std::endl;
        return ws;
    }
    catch(std::exception const& e) {
        std::cerr << "Setup error: " << e.what() << std::endl;
        delete ws;
        throw;
    }
}

void MEXC_Connection() {
    try {
        websocket::stream<beast::ssl_stream<tcp::socket>>* ws = nullptr;
        
        while (true) {
            try {
                std::string currentBase = g_baseInput;
                std::string currentQuote = g_quoteInput;
                std::string symbol = currentBase + currentQuote;
                g_shouldRefresh = false;

                // Clear existing orderbook data
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_bids.clear();
                    g_asks.clear();
                }

                // Safely close previous connection if it exists
                if (ws != nullptr) {
                    try {
                        ws->close(websocket::close_code::normal);
                        delete ws;
                    } catch (...) {
                        // Ignore any errors during cleanup
                    }
                    ws = nullptr;
                }

                // Create new connection
                ws = setupWebSocket();
                
                json subscriptionMsg = {
                    {"method", "SUBSCRIPTION"},
                    {"params", {
                        "spot@public.limit.depth.v3.api@" + symbol + "@20",
                        "spot@public.bookTicker.v3.api@" + symbol
                    }}
                };

                ws->write(net::buffer(subscriptionMsg.dump()));

                // Message handling loop
                beast::flat_buffer buffer;
                while(!g_shouldRefresh) {
                    try {
                        ws->read(buffer);
                        std::string msg = beast::buffers_to_string(buffer.data());
                        buffer.consume(buffer.size());

                        json j = json::parse(msg);
                        
                        // Handle depth stream data
                        if (j.contains("d") && j["d"].contains("asks") && j["d"].contains("bids")) {
                            auto data = j["d"];
                            std::lock_guard<std::mutex> lock(g_mutex);

                            if (data.contains("asks")) {
                                for (const auto& ask : data["asks"]) {
                                    std::string priceStr = ask["p"].get<std::string>();
                                    std::string volumeStr = ask["v"].get<std::string>();
                                    float price = std::stof(priceStr);
                                    float volume = std::stof(volumeStr);

                                    auto it = std::find_if(g_asks.begin(), g_asks.end(),
                                        [price](const auto& entry) { return entry.price == price; });

                                    if (it != g_asks.end()) {
                                        if (volume == 0) {
                                            g_asks.erase(it);
                                        } else {
                                            it->volume = volume;
                                            it->volumeStr = volumeStr;
                                        }
                                    } else if (volume != 0) {
                                        g_asks.push_back({priceStr, volumeStr, price, volume});
                                        std::sort(g_asks.begin(), g_asks.end(),
                                            [](const auto& a, const auto& b) {
                                                return a.price < b.price;
                                            });
                                    }
                                }
                            }

                            if (data.contains("bids")) {
                                for (const auto& bid : data["bids"]) {
                                    std::string priceStr = bid["p"].get<std::string>();
                                    std::string volumeStr = bid["v"].get<std::string>();
                                    float price = std::stof(priceStr);
                                    float volume = std::stof(volumeStr);

                                    auto it = std::find_if(g_bids.begin(), g_bids.end(),
                                        [price](const auto& entry) { return entry.price == price; });

                                    if (it != g_bids.end()) {
                                        if (volume == 0) {
                                            g_bids.erase(it);
                                        } else {
                                            it->volume = volume;
                                            it->volumeStr = volumeStr;
                                        }
                                    } else if (volume != 0) {
                                        g_bids.push_back({priceStr, volumeStr, price, volume});
                                        std::sort(g_bids.begin(), g_bids.end(),
                                            [](const auto& a, const auto& b) {
                                                return a.price > b.price;
                                            });
                                    }
                                }
                            }

                            // Maintain maximum size of 20 levels
                            if (g_asks.size() > 20) g_asks.resize(20);
                            if (g_bids.size() > 20) g_bids.resize(20);
                        }
                        // Handle book ticker stream data
                        else if (j.contains("d") && j["d"].contains("a") && j["d"].contains("b")) {
                            auto data = j["d"];
                            std::lock_guard<std::mutex> lock(g_mutex);

                            std::string ask_price = data["a"].get<std::string>();
                            std::string ask_volume = data["A"].get<std::string>();
                            std::string bid_price = data["b"].get<std::string>();
                            std::string bid_volume = data["B"].get<std::string>();

                            // Update best ask
                            auto ask_it = std::find_if(g_asks.begin(), g_asks.end(),
                                [&ask_price](const auto& entry) { 
                                    return std::abs(entry.price - std::stof(ask_price)) < 0.000001f; 
                                });

                            if (ask_it != g_asks.end()) {
                                ask_it->volume = std::stof(ask_volume);
                                ask_it->volumeStr = ask_volume;
                            } else {
                                g_asks.insert(g_asks.begin(), {
                                    ask_price,           // string price
                                    ask_volume,          // string volume
                                    std::stof(ask_price),  // float price
                                    std::stof(ask_volume)  // float volume
                                });
                                if (g_asks.size() > 20) g_asks.pop_back();
                            }

                            // Update best bid
                            auto bid_it = std::find_if(g_bids.begin(), g_bids.end(),
                                [&bid_price](const auto& entry) { 
                                    return std::abs(entry.price - std::stof(bid_price)) < 0.000001f; 
                                });

                            if (bid_it != g_bids.end()) {
                                bid_it->volume = std::stof(bid_volume);
                                bid_it->volumeStr = bid_volume;
                            } else {
                                g_bids.insert(g_bids.begin(), {
                                    bid_price,           // string price
                                    bid_volume,          // string volume
                                    std::stof(bid_price),  // float price
                                    std::stof(bid_volume)  // float volume
                                });
                                if (g_bids.size() > 20) g_bids.pop_back();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Error in message loop: " << e.what() << std::endl;
                        break;  // Break the inner loop to reconnect
                    }
                }

            } catch (const std::exception& e) {
                std::cerr << "Connection error: " << e.what() << std::endl;
                // Clean up if needed
                if (ws != nullptr) {
                    try {
                        ws->close(websocket::close_code::normal);
                        delete ws;
                        ws = nullptr;
                    } catch (...) {}
                }
                // Add a small delay before reconnecting
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
    }
}