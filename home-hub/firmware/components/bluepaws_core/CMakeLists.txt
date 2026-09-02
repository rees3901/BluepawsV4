idf_component_register(
    SRCS
        "src/map_engine.cpp"
        "src/cat_store.cpp"
        "src/cat_simulator.cpp"
        "src/hub_settings.cpp"
    INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
