# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— business/sqlite_db（数据库封装模块）
# =============================================================================
# 【模块定位】
#   业务逻辑层数据持久化模块，负责：
#   - SQLite3 数据库连接管理
#   - 三张表 CRUD：chat_history / kb_files / perf_log
#   - 对话历史、知识库索引、性能日志的增删改查
#
# 【本 CMakeLists.txt 作用】
#   1. 编译 sqlite_db.c → libsqlite_db.a 静态库
#   2. 可选编译 test_sqlite_db 测试程序（BUILD_TESTS=ON 时）
#
# 【依赖说明】
#   需要系统安装 SQLite3 开发库（libsqlite3-dev），链接 -lsqlite3。
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
file(GLOB SQLITE_DB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER SQLITE_DB_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libsqlite_db.a）
# -----------------------------------------------------------------------------
if(SQLITE_DB_SOURCES)
    add_library(sqlite_db STATIC ${SQLITE_DB_SOURCES})
else()
    add_library(sqlite_db STATIC)
endif()

# 头文件路径对外暴露
target_include_directories(sqlite_db INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接 SQLite3
# 优先用 CMake 的 IMPORTED target（SQLite::SQLite3），找不到则回退 -lsqlite3
if(TARGET SQLite::SQLite3)
    target_link_libraries(sqlite_db PUBLIC SQLite::SQLite3)
elseif(SQLite3_FOUND)
    target_link_libraries(sqlite_db PUBLIC ${SQLITE3_LIBRARIES})
    target_include_directories(sqlite_db PUBLIC ${SQLITE3_INCLUDE_DIRS})
else()
    # 兜底：直接链接系统 sqlite3 库
    target_link_libraries(sqlite_db PUBLIC sqlite3)
endif()


# -----------------------------------------------------------------------------
# 三、测试程序
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    file(GLOB SQLITE_DB_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${SQLITE_DB_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        # 测试程序用源码方式编译进来
        target_sources(${test_name} PRIVATE ${SQLITE_DB_SOURCES})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        # 链接 sqlite3
        if(TARGET SQLite::SQLite3)
            target_link_libraries(${test_name} PRIVATE SQLite::SQLite3)
        elseif(SQLite3_FOUND)
            target_link_libraries(${test_name} PRIVATE ${SQLITE3_LIBRARIES})
        else()
            target_link_libraries(${test_name} PRIVATE sqlite3)
        endif()
        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()
