add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_lut3d_bounds
                SOURCES test_lut3d_bounds.c
                LINK_LIBRARIES lib_darktable cmocka)

add_cmocka_test(test_colorin_hsm
                SOURCES test_colorin_hsm.c
                LINK_LIBRARIES lib_darktable cmocka)

add_executable(test_magiceraser_result
               test_magiceraser_result.c
               ${CMAKE_SOURCE_DIR}/src/iop/magiceraser_result.c)
target_link_libraries(test_magiceraser_result ${Glib_LIBRARIES})
add_test(NAME test_magiceraser_result COMMAND test_magiceraser_result)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_darktable)
    _copy_required_library(test_lut3d_bounds lib_darktable)
    _copy_required_library(test_colorin_hsm lib_darktable)
endif(WIN32)
