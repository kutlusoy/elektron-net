cmake_minimum_required(VERSION 3.22)
project(elektron_miner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(elektron_miner miner.cpp)
target_link_libraries(elektron_miner PRIVATE
    Threads::Threads
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(elektron_miner PRIVATE ws2_32)
endif()
