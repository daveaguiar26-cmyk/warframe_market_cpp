cmake_minimum_required(VERSION 3.16)
project(warframe_market_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Incluir os módulos Widgets e Network do Qt
find_package(Qt6 COMPONENTS Widgets Network REQUIRED)

add_executable(warframe_app
    main.cpp
    wfmauth.cpp
    wfdata.cpp
)

# Ligar as bibliotecas do Qt ao executável
target_link_libraries(warframe_app PRIVATE Qt6::Widgets Qt6::Network)
