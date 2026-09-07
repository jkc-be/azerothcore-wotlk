execute_process(COMMAND "${CONSUMER}"
  RESULT_VARIABLE CONSUMER_RESULT
  OUTPUT_VARIABLE CONSUMER_STDOUT
  ERROR_VARIABLE CONSUMER_STDERR)
if("${CONSUMER_RESULT}" STREQUAL "0")
  message(FATAL_ERROR "Mismatched consumer unexpectedly passed the statement-layout guard")
endif()
set(CONSUMER_OUTPUT "${CONSUMER_STDOUT}${CONSUMER_STDERR}")
if(NOT CONSUMER_OUTPUT MATCHES "mod-alles CharacterDatabase statement layout mismatch")
  message(FATAL_ERROR "Consumer failed without the expected layout diagnostic: ${CONSUMER_OUTPUT}")
endif()
