# CPack pre-build script (macOS): sign the staged app bundle after everything is installed into it,
# so the signature covers the final contents. Ad-hoc ("-") unless SCULPTFORGE_CODESIGN_IDENTITY is set
# in the environment (e.g. "Developer ID Application: ..." from a CI secret).
set(identity "$ENV{SCULPTFORGE_CODESIGN_IDENTITY}")
if(identity STREQUAL "")
  set(identity "-")
endif()
file(GLOB_RECURSE plists "${CPACK_TEMPORARY_DIRECTORY}/*SculptForge.app/Contents/Info.plist")
foreach(plist ${plists})
  get_filename_component(contents "${plist}" DIRECTORY)
  get_filename_component(app "${contents}" DIRECTORY)
  message(STATUS "Signing ${app} (${identity})")
  execute_process(COMMAND codesign --force --deep --options runtime --timestamp=none --sign "${identity}" "${app}"
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "codesign failed for ${app}")
  endif()
endforeach()
