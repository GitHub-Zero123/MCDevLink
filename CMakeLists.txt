cmake_minimum_required(VERSION 3.24)

project(MC_DEV_LINK LANGUAGES CXX C)

# MCDevLink 通用MC开发者调试协议后端
# CPP标准：20
# 核心库不直接写死MT/MD运行库链接声明 由上层工程根据自身需求决定