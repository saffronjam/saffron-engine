add_executable(se source/main.cpp)

target_compile_features(se PRIVATE cxx_std_20)
target_link_libraries(se PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(se PRIVATE JSON_NOEXCEPTION)
