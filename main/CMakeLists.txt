idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")

# Force linker to wrap the internal function
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=wdevProcessRxSucDataAll")
