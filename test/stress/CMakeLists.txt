# Copyright (c) 2026 LG Electronics, Inc. and webOS-ports project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

project(db8-stress CXX)

include_directories(${CMAKE_SOURCE_DIR}/inc)

set(DB_BACKEND_WRAPPER_SOURCES_CPP "")
foreach(filename ${DB_BACKEND_WRAPPER_SOURCES})
    set(DB_BACKEND_WRAPPER_SOURCES_CPP ${DB_BACKEND_WRAPPER_SOURCES_CPP} "${CMAKE_SOURCE_DIR}/${filename}")
endforeach ()

add_executable(${PROJECT_NAME} StressRunner.cpp ${DB_BACKEND_WRAPPER_SOURCES_CPP})
target_link_libraries(${PROJECT_NAME}
    mojocore
    mojodb
    ${DB_BACKEND_LIB}
    atomic
    pthread
)

# quick smoke run as part of "make check"; pin the engine in case several
# backends are compiled in
list(GET WEBOS_DB8_BACKEND 0 STRESS_ENGINE)
add_test(NAME db8-stress-smoke COMMAND ${PROJECT_NAME} --seconds 5 --writers 2 --readers 2 --objects 200)
set_tests_properties(db8-stress-smoke PROPERTIES ENVIRONMENT "MOJODB_ENGINE=${STRESS_ENGINE}")

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}
        DESTINATION ${WEBOS_INSTALL_LIBDIR}/${CMAKE_PROJECT_NAME}/tests
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/db8-stress-luna.sh
        DESTINATION ${WEBOS_INSTALL_LIBDIR}/${CMAKE_PROJECT_NAME}/tests
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ)
