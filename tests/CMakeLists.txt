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

add_executable(message-formatter-test
    MessageFormatterTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/MessageFormatter.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiInfo.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiMap.cpp"
)

target_include_directories(message-formatter-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(message-formatter-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME message-formatter-test COMMAND message-formatter-test)
set_tests_properties(message-formatter-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
