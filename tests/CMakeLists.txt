# Smoke test: link psio + psitri together via the psitri_multiindex
# interface target and confirm both libraries can be exercised in the
# same translation unit.
add_executable(psitri_multiindex_smoke smoke_test.cpp)
target_link_libraries(psitri_multiindex_smoke PRIVATE psitri_multiindex)

add_test(NAME psitri_multiindex_smoke COMMAND psitri_multiindex_smoke)
