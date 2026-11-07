# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
if(NOT SOURCE_DIR OR NOT EXPECTED_SHA OR NOT GIT_EXECUTABLE)
  message(FATAL_ERROR "Release source verification requires source directory, Git and configured SHA")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _head_result
  OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=all
  WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _status_result
  OUTPUT_VARIABLE _status OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _head_result EQUAL 0 OR NOT _status_result EQUAL 0 OR
   NOT _head STREQUAL EXPECTED_SHA OR NOT _status STREQUAL "")
  message(FATAL_ERROR
    "Release source no longer matches its configured clean Git commit. Commit changes and reconfigure before building.")
endif()
