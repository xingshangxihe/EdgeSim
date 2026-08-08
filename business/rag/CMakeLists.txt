# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— business/rag（本地 RAG 知识库检索模块）
# =============================================================================
# 【模块定位】
#   业务逻辑层轻量 RAG 检索模块，负责：
#   - 遍历 sqlite_db 的 kb_files 表
#   - 读取 TXT/MD 文件内容，做关键词命中打分
#   - 返回与查询最相关的 Top N 片段（供拼接为 LLM 上下文）
#
# 【本 CMakeLists.txt 作用】
#   1. 编译 rag.c → librag.a 静态库
#   2. 可选编译 test_rag 测试程序（BUILD_TESTS=ON 时）
#
# 【依赖说明】
#   rag.c 调用 kb_query（sqlite_db 模块），因此：
#   - 需要 business/sqlite_db 的头文件路径
#   - 链接 sqlite_db 库（其 PUBLIC 依赖会把 sqlite3 一并传播）
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
file(GLOB RAG_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER RAG_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（librag.a）
# -----------------------------------------------------------------------------
if(RAG_SOURCES)
    add_library(rag STATIC ${RAG_SOURCES})
else()
    add_library(rag STATIC)
endif()

# rag.h 对外暴露（本模块头文件）
target_include_directories(rag INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# rag.c 需要 sqlite_db.h（kb_files 表结构 + kb_query 接口）
target_include_directories(rag PUBLIC
    "${CMAKE_SOURCE_DIR}/business/sqlite_db"
)

# rag 依赖 sqlite_db 库（rag.c 调用 kb_query）。
# sqlite_db 的 PUBLIC 依赖会连带传播 sqlite3 库与头文件路径。
if(TARGET sqlite_db)
    target_link_libraries(rag PUBLIC sqlite_db)
endif()


# -----------------------------------------------------------------------------
# 三、测试程序 test_rag
# -----------------------------------------------------------------------------
# 测试逻辑（test_rag.c）：
#   1. 在 /tmp 创建临时知识库 txt 文件
#   2. db_init(":memory:") 建内存库
#   3. kb_add 登记文件
#   4. rag_retrieve 检索，校验结果命中
if(BUILD_TESTS)
    file(GLOB RAG_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${RAG_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        # 测试程序用源码方式编入 rag.c + sqlite_db.c（与 ui_lvgl 测试同模式）
        target_sources(${test_name} PRIVATE
            ${RAG_SOURCES}
            "${CMAKE_SOURCE_DIR}/business/sqlite_db/sqlite_db.c"
        )
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            "${CMAKE_SOURCE_DIR}/business/sqlite_db"
        )
        # 链接 sqlite3（sqlite_db.c 源码依赖）
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
