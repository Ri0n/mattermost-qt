add_executable(posts-list-item-test
    PostsListItemTest.cpp
)

target_include_directories(posts-list-item-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(posts-list-item-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME posts-list-item-test COMMAND posts-list-item-test)
