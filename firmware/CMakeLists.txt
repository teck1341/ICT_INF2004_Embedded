cmake_minimum_required(VERSION 3.16)
project(inf2004_portable_modules LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(CORE_SANITIZERS "Enable ASan and UBSan for host testing" OFF)

# Host build of the hardware-independent modules in ../app_program (the same
# list as run_tests.sh and scripts/robot_paths.py). Board/RTOS adapters build
# only with the project template's build_make/; this never produces firmware.
set(app_dir ${CMAKE_CURRENT_SOURCE_DIR}/../app_program)
set(app_dirs ${app_dir} ${app_dir}/subsystem1 ${app_dir}/subsystem2
    ${app_dir}/subsystem3 ${app_dir}/subsystem4 ${app_dir}/subsystem5)
set(portable_sources
    ${app_dir}/bitpack.c ${app_dir}/core.c
    ${app_dir}/subsystem1/mqttcmd.c ${app_dir}/subsystem1/telemetry.c
    ${app_dir}/subsystem2/motion.c
    ${app_dir}/subsystem3/barcode.c ${app_dir}/subsystem3/line_ctrl.c
    ${app_dir}/subsystem4/terrain.c
    ${app_dir}/subsystem5/scanner.c ${app_dir}/subsystem5/vehicle.c)
file(GLOB test_sources CONFIGURE_DEPENDS tests/test_*_main.c)
add_library(robot_modules STATIC ${portable_sources})
target_include_directories(robot_modules PUBLIC ${app_dirs})
set(check_targets robot_modules)

# LeakSanitizer is off by project choice until it has been tried on the CI
# runner; configure with ROBOT_LSAN=1 in the environment to enable it.
if("$ENV{ROBOT_LSAN}" STREQUAL "1")
    set(robot_detect_leaks 1)
else()
    set(robot_detect_leaks 0)
endif()

enable_testing()
foreach(test_source IN LISTS test_sources)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE robot_modules m)
    list(APPEND check_targets ${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    if(CORE_SANITIZERS)
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT
            "ASAN_OPTIONS=detect_leaks=${robot_detect_leaks}:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endforeach()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target IN LISTS check_targets)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
            -Wundef -Wcast-qual -Wwrite-strings -Wswitch-enum
            -Wnull-dereference)
        if(CORE_SANITIZERS)
            target_compile_options(${target} PRIVATE -g -fno-omit-frame-pointer
                -fsanitize=address,undefined,float-cast-overflow
                -fno-sanitize-recover=all)
            target_link_options(${target} INTERFACE
                -fsanitize=address,undefined,float-cast-overflow
                -fno-sanitize-recover=all)
            get_target_property(target_kind ${target} TYPE)
            if(target_kind STREQUAL "EXECUTABLE")
                target_link_options(${target} PRIVATE
                    -fsanitize=address,undefined,float-cast-overflow
                    -fno-sanitize-recover=all)
            endif()
        endif()
    endforeach()
endif()
