idf_component_register(
    SRCS "shtc3_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board shtc3 esp_timer task_config
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
