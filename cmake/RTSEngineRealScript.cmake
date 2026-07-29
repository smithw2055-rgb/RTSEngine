include(CMakeParseArguments)

# Generate a RealScript C++17 AOT static library and attach the engine-side
# scripting runtime needed by host adapters. The generated manifest path is
# exposed through RTSENGINE_SCRIPT_AOT_MANIFEST.
function(rtsengine_add_script_aot target)
    if(NOT RTSENGINE_ENABLE_REALSCRIPT)
        message(FATAL_ERROR
            "rtsengine_add_script_aot requires RTSENGINE_ENABLE_REALSCRIPT=ON")
    endif()
    if(NOT COMMAND realscript_add_aot_library)
        message(FATAL_ERROR
            "The pinned RealScript SDK does not expose realscript_add_aot_library")
    endif()

    set(options NO_LINE_DIRECTIVES)
    set(one_value_args
        PROGRAM_NAME
        CPP_NAMESPACE
        QUERY_SYMBOL
        OUTPUT_DIRECTORY
        OPT_LEVEL
    )
    set(multi_value_args SOURCES)
    cmake_parse_arguments(
        RTSAOT
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )
    if(RTSAOT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rtsengine_add_script_aot(${target}) received unknown arguments: "
            "${RTSAOT_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT RTSAOT_SOURCES)
        message(FATAL_ERROR
            "rtsengine_add_script_aot(${target}) requires SOURCES")
    endif()

    set(arguments SOURCES ${RTSAOT_SOURCES})
    if(RTSAOT_PROGRAM_NAME)
        list(APPEND arguments PROGRAM_NAME "${RTSAOT_PROGRAM_NAME}")
    endif()
    if(RTSAOT_CPP_NAMESPACE)
        list(APPEND arguments CPP_NAMESPACE "${RTSAOT_CPP_NAMESPACE}")
    endif()
    if(RTSAOT_QUERY_SYMBOL)
        list(APPEND arguments QUERY_SYMBOL "${RTSAOT_QUERY_SYMBOL}")
    endif()
    if(RTSAOT_OUTPUT_DIRECTORY)
        list(APPEND arguments OUTPUT_DIRECTORY "${RTSAOT_OUTPUT_DIRECTORY}")
    endif()
    if(NOT RTSAOT_OPT_LEVEL STREQUAL "")
        list(APPEND arguments OPT_LEVEL "${RTSAOT_OPT_LEVEL}")
    endif()
    if(RTSAOT_NO_LINE_DIRECTIVES)
        list(APPEND arguments NO_LINE_DIRECTIVES)
    endif()

    realscript_add_aot_library(${target} ${arguments})
    target_link_libraries(${target} PUBLIC RTSEngine::Scripting)
    target_compile_definitions(${target} PUBLIC RTSENGINE_SCRIPT_AOT=1)
    set_target_properties(${target} PROPERTIES FOLDER "RTSEngine/ScriptAot")

    get_target_property(manifest ${target} REALSCRIPT_AOT_MANIFEST)
    set_property(
        TARGET ${target}
        PROPERTY RTSENGINE_SCRIPT_AOT_MANIFEST "${manifest}")
endfunction()
