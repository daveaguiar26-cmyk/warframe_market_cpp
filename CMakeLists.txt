cmake_minimum_required(VERSION 3.16)
project(warframe_market_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Widgets e Network para a app; WebSockets para o estado da conta, que o
# warframe.market só expõe por wss:// (o v2 rejeita o campo num PATCH /me).
find_package(Qt6 COMPONENTS Widgets Network WebSockets REQUIRED)

add_executable(warframe_app
    main.cpp
    wfmauth.cpp
    wfdata.cpp
)

# Ligar as bibliotecas do Qt ao executável
target_link_libraries(warframe_app PRIVATE Qt6::Widgets Qt6::Network Qt6::WebSockets)