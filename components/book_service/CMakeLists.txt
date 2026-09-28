idf_component_register(
    SRCS "book_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash storage_service
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
