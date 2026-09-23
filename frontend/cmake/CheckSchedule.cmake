# The schedule is the application's wiring, and it is stated in three places: main.cpp
# builds it, docs/architecture.md explains it, and the test harness mirrors it. Prose
# has already lost that argument once -- two systems were added and the doc gained one.
#
# So the order and membership are checked mechanically against a golden file. The
# annotated table in docs/architecture.md still says what each system is FOR; this says
# what actually runs, and in what order.
execute_process(COMMAND ${RGV_BIN} --schedule OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "rgv --schedule failed with ${rc}")
endif()
file(READ ${GOLDEN} expected)
string(STRIP "${actual}" actual)
string(STRIP "${expected}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR
    "The system schedule changed.\n\n"
    "--- ${GOLDEN}\n${expected}\n\n"
    "+++ rgv --schedule\n${actual}\n\n"
    "If the change is intended, update docs/schedule.txt AND the annotated table in "
    "docs/architecture.md, so the two cannot drift apart again.")
endif()
