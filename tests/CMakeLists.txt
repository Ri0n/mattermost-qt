add_executable(long-list-widget-test
    LongListWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/widgets/LongListWidget.cpp"
)

target_include_directories(long-list-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(long-list-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME long-list-widget-test COMMAND long-list-widget-test)
set_tests_properties(long-list-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(long-list-widget-removal-test
    LongListWidgetRemovalTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/widgets/LongListWidget.cpp"
)

target_include_directories(long-list-widget-removal-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(long-list-widget-removal-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME long-list-widget-removal-test COMMAND long-list-widget-removal-test)
set_tests_properties(long-list-widget-removal-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

add_executable(message-content-widget-test
    MessageContentWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/MessageContentWidget.cpp"
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/MessageFormatter.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiInfo.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/emoji/EmojiMap.cpp"
)

target_include_directories(message-content-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(message-content-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
        qsourcehighlite
)

add_test(NAME message-content-widget-test COMMAND message-content-widget-test)
set_tests_properties(message-content-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(post-attachment-list-widget-test
    PostAttachmentListWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/post/attachments/PostAttachmentListWidget.cpp"
)

target_include_directories(post-attachment-list-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-attachment-list-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME post-attachment-list-widget-test COMMAND post-attachment-list-widget-test)
set_tests_properties(post-attachment-list-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(channel-activity-tracker-test
    ChannelActivityTrackerTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/ChannelActivityTracker.cpp"
)

target_include_directories(channel-activity-tracker-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(channel-activity-tracker-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME channel-activity-tracker-test COMMAND channel-activity-tracker-test)
set_tests_properties(channel-activity-tracker-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(backend-user-test
    BackendUserTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendUser.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendUserProps.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendNotifyPreps.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/types/BackendTimeZone.cpp"
)

target_include_directories(backend-user-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(backend-user-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME backend-user-test COMMAND backend-user-test)
set_tests_properties(backend-user-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(sidebar-item-delegate-test
    SidebarItemDelegateTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/channel-tree/ChannelItemDelegate.cpp"
)

target_include_directories(sidebar-item-delegate-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(sidebar-item-delegate-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME sidebar-item-delegate-test COMMAND sidebar-item-delegate-test)
set_tests_properties(sidebar-item-delegate-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(post-window-selection-test
    PostWindowSelectionTest.cpp
)

target_include_directories(post-window-selection-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-window-selection-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME post-window-selection-test COMMAND post-window-selection-test)

add_executable(post-navigation-window-test
    PostNavigationWindowTest.cpp
)

target_include_directories(post-navigation-window-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(post-navigation-window-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME post-navigation-window-test COMMAND post-navigation-window-test)

add_executable(message-text-edit-widget-test
    MessageTextEditWidgetTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/outgoing-post/MessageTextEditWidget.cpp"
)

target_include_directories(message-text-edit-widget-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(message-text-edit-widget-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets
)

add_test(NAME message-text-edit-widget-test COMMAND message-text-edit-widget-test)
set_tests_properties(message-text-edit-widget-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(quoted-reply-format-test
    QuotedReplyFormatTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/chat-area/QuotedReplyFormat.cpp"
)

target_include_directories(quoted-reply-format-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(quoted-reply-format-test
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_test(NAME quoted-reply-format-test COMMAND quoted-reply-format-test)
