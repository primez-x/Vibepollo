cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED ROOT OR NOT IS_DIRECTORY "${ROOT}/src")
  message(FATAL_ERROR "ROOT must name the Vibepollo repository")
endif()
if(NOT DEFINED INVENTORY OR NOT EXISTS "${INVENTORY}")
  message(FATAL_ERROR "HDR runtime inventory manifest is missing")
endif()
if(NOT DEFINED ROUTE_INVENTORY OR NOT EXISTS "${ROUTE_INVENTORY}")
  message(FATAL_ERROR "HDR route inventory manifest is missing")
endif()
if(NOT DEFINED WRITER_INVENTORY OR NOT EXISTS "${WRITER_INVENTORY}")
  message(FATAL_ERROR "HDR runtime writer inventory manifest is missing")
endif()

file(STRINGS "${WRITER_INVENTORY}" writer_lines)
set(writer_files "")
foreach(writer_line IN LISTS writer_lines)
  string(STRIP "${writer_line}" writer_line)
  if(writer_line STREQUAL "" OR writer_line MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" writer_fields "${writer_line}")
  list(GET writer_fields 0 writer_file)
  list(APPEND writer_files "${writer_file}")
endforeach()
list(REMOVE_DUPLICATES writer_files)

file(GLOB_RECURSE HDR_SOURCES LIST_DIRECTORIES false
  "${ROOT}/src/*.cpp"
  "${ROOT}/src/*.h")

file(GLOB CONFIG_HTTP_SOURCES LIST_DIRECTORIES false "${ROOT}/src/confighttp*.cpp")
foreach(config_http_source IN LISTS CONFIG_HTTP_SOURCES)
  file(READ "${config_http_source}" config_http_text)
  if(config_http_text MATCHES "request->|parse_query_string|request->query_string")
    file(RELATIVE_PATH relative_config_http_source "${ROOT}" "${config_http_source}")
    message(FATAL_ERROR
      "Configuration HTTP source crosses the request adapter or parses raw query data: ${relative_config_http_source}")
  endif()
endforeach()

set(feature_files "")
foreach(source_file IN LISTS HDR_SOURCES)
  file(READ "${source_file}" source_text)
  file(RELATIVE_PATH relative_source "${ROOT}" "${source_file}")
  file(TO_CMAKE_PATH "${relative_source}" relative_source)

  # The config singleton is the storage implementation. The bridge is the
  # only production publication boundary; all other callers must use the
  # manager-owned bridge instead of the low-level setter/clear functions.
  if(NOT relative_source STREQUAL "src/config.cpp")
    if(source_text MATCHES "config::set_runtime_config_overrides[ \\t\\r\\n]*\\(")
      list(FIND writer_files "${relative_source}" writer_index)
      if(writer_index EQUAL -1)
        message(FATAL_ERROR
          "Unclassified direct runtime override setter caller: ${relative_source}")
      endif()
    endif()
    if(source_text MATCHES "config::clear_runtime_config_overrides[ \\t\\r\\n]*\\(")
      list(FIND writer_files "${relative_source}" writer_index)
      if(writer_index EQUAL -1)
        message(FATAL_ERROR
          "Unclassified direct runtime override clearer caller: ${relative_source}")
      endif()
    endif()
  endif()

  if(source_text MATCHES "config::(set_runtime_config_overrides|clear_runtime_config_overrides)[ \\t\\r\\n]*\\(")
    list(FIND writer_files "${relative_source}" writer_index)
    if(writer_index EQUAL -1)
      message(FATAL_ERROR
        "Runtime override writer is not listed in the writer inventory: ${relative_source}")
    endif()
  endif()

  if(NOT relative_source STREQUAL "src/config.cpp"
     AND source_text MATCHES "g_runtime_config_overrides[ \\t\\r\\n]*(=|\\.clear[ \\t\\r\\n]*\\()")
    message(FATAL_ERROR
      "Direct runtime override map mutation outside config.cpp: ${relative_source}")
  endif()

  if(source_text MATCHES "rtx_hdr_peak_brightness")
    list(APPEND feature_files "${relative_source}")
  endif()
endforeach()

file(STRINGS "${INVENTORY}" inventory_lines)
set(inventory_files "")
foreach(inventory_line IN LISTS inventory_lines)
  string(STRIP "${inventory_line}" inventory_line)
  if(inventory_line STREQUAL "" OR inventory_line MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" inventory_fields "${inventory_line}")
  list(GET inventory_fields 0 inventory_file)
  list(APPEND inventory_files "${inventory_file}")
  list(FIND feature_files "${inventory_file}" feature_index)
  if(feature_index EQUAL -1)
    message(FATAL_ERROR
      "HDR runtime inventory entry has no matching source occurrence: ${inventory_file}")
  endif()
endforeach()

list(REMOVE_DUPLICATES feature_files)
list(REMOVE_DUPLICATES inventory_files)
foreach(feature_file IN LISTS feature_files)
  list(FIND inventory_files "${feature_file}" inventory_index)
  if(inventory_index EQUAL -1)
    message(FATAL_ERROR
      "Unclassified rtx_hdr_peak_brightness source occurrence: ${feature_file}")
  endif()
endforeach()

# Route files are inventoried separately because they cross the transport
# library boundary. They must construct the bounded request view and may not
# parse the raw query string themselves.
file(STRINGS "${ROUTE_INVENTORY}" route_lines)
foreach(route_line IN LISTS route_lines)
  string(STRIP "${route_line}" route_line)
  if(route_line STREQUAL "" OR route_line MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" route_fields "${route_line}")
  list(GET route_fields 0 route_file)
  list(REMOVE_AT route_fields 0)
  set(route_path "${ROOT}/${route_file}")
  if(NOT EXISTS "${route_path}")
    message(FATAL_ERROR "Inventoried route source is missing: ${route_file}")
  endif()
  file(READ "${route_path}" route_text)
  if(route_text MATCHES "parse_query_string|request->query_string")
    message(FATAL_ERROR "Route source parses the raw query string: ${route_file}")
  endif()
  foreach(required_token IN LISTS route_fields)
    if(NOT route_text MATCHES "${required_token}")
      message(FATAL_ERROR
        "Route source ${route_file} is missing required adapter/inventory token: ${required_token}")
    endif()
  endforeach()
endforeach()

message(STATUS "HDR runtime writer, feature-key, and route inventories are consistent")
