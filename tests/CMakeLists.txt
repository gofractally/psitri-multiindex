# Smoke test: link psio + psitri together via the psitri_multiindex
# interface target and confirm both libraries can be exercised in the
# same translation unit.
add_executable(psitri_multiindex_smoke smoke_test.cpp)
target_link_libraries(psitri_multiindex_smoke PRIVATE psitri_multiindex)
add_test(NAME psitri_multiindex_smoke COMMAND psitri_multiindex_smoke)

# Catch2-driven unit tests.
find_package(Catch2 3 REQUIRED)

add_executable(psitri_multiindex_tests
   key_codec_tests.cpp
   table_basic_tests.cpp
   table_secondary_tests.cpp
   table_iteration_tests.cpp
   table_subtx_tests.cpp)
target_link_libraries(psitri_multiindex_tests
   PRIVATE
      psitri_multiindex
      Catch2::Catch2WithMain)
add_test(NAME psitri_multiindex_tests COMMAND psitri_multiindex_tests)
