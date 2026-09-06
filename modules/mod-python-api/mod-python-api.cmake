# Direct provider calls require both modules in the same static target.
ModuleNameToVariable("mod-python-api" PYTHON_API_LINKAGE_VARIABLE)
if(NOT ${PYTHON_API_LINKAGE_VARIABLE} STREQUAL "disabled"
    AND EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Bot/PlayerbotAI.h")
  ModuleNameToVariable("mod-playerbots" PYTHON_API_PROVIDER_VARIABLE)
  if(NOT ${PYTHON_API_LINKAGE_VARIABLE} STREQUAL "static"
      OR NOT ${PYTHON_API_PROVIDER_VARIABLE} STREQUAL "static")
    message(FATAL_ERROR "mod-python-api and mod-playerbots must both use static module linkage")
  endif()
endif()
