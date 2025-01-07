cmake_minimum_required(VERSION 3.30)
project(MEXC_OrderBook_Spot)

set(CMAKE_CXX_STANDARD 26)


# Include nlohmann_json
include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# Add raylib
FetchContent_Declare(raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 4.5.0)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # don't build the supplied examples
FetchContent_MakeAvailable(raylib)


find_path(UWEBSOCKETS_INCLUDE_DIRS "uwebsockets/App.h")
# Find required packages
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_library(USOCKETS_LIBRARY NAMES uSockets)

# Find Zlib
find_package(ZLIB REQUIRED)

# Add the executable
add_executable(MEXC_OrderBook_Spot
        main.cpp
        raylib_setup.cpp

        #If you want to compile spot. then comment: then remove spot .
        #        Programs/OrderBook_MEXC_USDT_Futures.cpp
        #        Programs/OrderBook_MEXC_USDT_Futures.h


        #Programs/OrderBook_MEXC_Spot.cpp
        #Programs/OrderBook_MEXC_Spot.h
        Programs/OrderBook_MEXC_USDT_Futures.cpp
        Programs/OrderBook_MEXC_USDT_Futures.h

)

# Link required libraries
target_link_libraries(MEXC_OrderBook_Spot
        PRIVATE
        Boost::system    # Link Boost.System (required by Beast)
        OpenSSL::SSL     # OpenSSL support
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        raylib
        ${USOCKETS_LIBRARY} ZLIB::ZLIB
)

# Add Boost include directories (needed for header-only libraries like Beast)
target_include_directories(MEXC_OrderBook_Spot
        PRIVATE ${Boost_INCLUDE_DIRS}
)