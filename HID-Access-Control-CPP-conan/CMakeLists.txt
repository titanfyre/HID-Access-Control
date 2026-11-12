cmake_minimum_required(VERSION 3.24)
project(HIDAccessControlSDK VERSION 0.1.0 LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This SDK requires Windows x64. No driver is installed by this build.")
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
add_library(TitanHIDAccessControl STATIC
  "sdk/SDK/HidAccess.cpp"
)
add_library(HIDAccessControlSDK::sdk ALIAS TitanHIDAccessControl)
set_target_properties(TitanHIDAccessControl PROPERTIES EXPORT_NAME sdk)
target_compile_features(TitanHIDAccessControl PUBLIC cxx_std_17)
target_compile_definitions(TitanHIDAccessControl PUBLIC )
target_compile_definitions(TitanHIDAccessControl PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(MSVC)
  target_compile_options(TitanHIDAccessControl PRIVATE /utf-8 /EHsc /W4)
endif()
target_include_directories(TitanHIDAccessControl PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
  "$<INSTALL_INTERFACE:include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/Protocol>"
  "$<INSTALL_INTERFACE:include/Protocol>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/SDK>"
  "$<INSTALL_INTERFACE:include/SDK>"
)
target_link_libraries(TitanHIDAccessControl PUBLIC advapi32 comctl32 comdlg32 crypt32 gdi32 newdev ole32 setupapi shell32 shlwapi user32 uxtheme windowscodecs wintrust)
install(TARGETS TitanHIDAccessControl EXPORT HIDAccessControlSDKTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY sdk/ DESTINATION include FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
install(FILES LICENSE DESTINATION share/hid-access-control)
install(EXPORT HIDAccessControlSDKTargets NAMESPACE HIDAccessControlSDK:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HIDAccessControlSDK)
configure_file("HIDAccessControlSDKConfig.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/HIDAccessControlSDKConfig.cmake" @ONLY)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/HIDAccessControlSDKConfigVersion.cmake" VERSION 0.1.0 COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/HIDAccessControlSDKConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/HIDAccessControlSDKConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HIDAccessControlSDK)
