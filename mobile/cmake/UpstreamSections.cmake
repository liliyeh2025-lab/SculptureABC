# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#
# Uses upstream SculptForge's own target definitions instead of a copied file list: reads the repository's root
# CMakeLists.txt, cuts out a section between two of its "# --- ..." headings, rewrites the paths so they point at the
# repository root (they are written relative to it), and runs that text here. When upstream adds a source file to
# sculptforge_core, the next configure picks it up; when upstream reorganises a heading, configure stops with a
# message naming the heading to look for.
#
#   sculpture_upstream_section(<begin heading> <end heading> [<code appended to the cut>])
#
# Needs SCULPTURE_REPO_ROOT (absolute). Sets SCULPTFORGE_UPSTREAM_VERSION (upstream's project version).

file(READ ${SCULPTURE_REPO_ROOT}/CMakeLists.txt SCULPTURE_UPSTREAM_CMAKE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SCULPTURE_REPO_ROOT}/CMakeLists.txt)
if(NOT SCULPTURE_UPSTREAM_CMAKE MATCHES "project\\(SculptForge VERSION ([0-9.]+)")
  message(FATAL_ERROR "Upstream CMakeLists.txt: no 'project(SculptForge VERSION x.y.z' line")
endif()
set(SCULPTFORGE_UPSTREAM_VERSION ${CMAKE_MATCH_1})

function(sculpture_upstream_section begin end)
  string(FIND "${SCULPTURE_UPSTREAM_CMAKE}" "${begin}" b)
  if(b EQUAL -1)
    message(FATAL_ERROR "Upstream CMakeLists.txt: heading '${begin}' not found (mobile/cmake/UpstreamSections.cmake)")
  endif()
  string(SUBSTRING "${SCULPTURE_UPSTREAM_CMAKE}" ${b} -1 rest)
  string(FIND "${rest}" "${end}" e)
  if(e EQUAL -1)
    message(FATAL_ERROR "Upstream CMakeLists.txt: heading '${end}' not found after '${begin}'")
  endif()
  string(SUBSTRING "${rest}" 0 ${e} code)

  # Test data: the web build reads it through a mounted folder (SCULPTURE_TEST_DATA_DIR), not the Windows path.
  if(DEFINED SCULPTURE_TEST_DATA_DIR)
    string(REPLACE "SF_TEST_DATA_DIR=\"\${CMAKE_CURRENT_SOURCE_DIR}/tests/data\""
                   "SF_TEST_DATA_DIR=\"\${SCULPTURE_TEST_DATA_DIR}\"" code "${code}")
  endif()
  string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}" "\${SCULPTURE_REPO_ROOT}" code "${code}")
  string(REPLACE "\${PROJECT_VERSION}" "\${SCULPTFORGE_UPSTREAM_VERSION}" code "${code}")
  # Relative paths (src/..., third_party/..., the bare `src` include directory) -> repository root. Run twice:
  # a separator matched as one path's end can't also start the next.
  foreach(pass 1 2)
    string(REGEX REPLACE "([ \t\n(])(src|third_party|cmake|tests|assets)([/ \t\n)])"
                         "\\1\${SCULPTURE_REPO_ROOT}/\\2\\3" code "${code}")
  endforeach()
  cmake_language(EVAL CODE "${code}
${ARGN}")
endfunction()
