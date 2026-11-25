# Setting CWD as later on it's not possible if called from outside
set_property(GLOBAL PROPERTY
    VULKAN_COMPILE_SHADER_CWD_PROPERTY "${CMAKE_CURRENT_LIST_DIR}")

function(vulkan_compile_shader)
     find_program(GLS_LANG_VALIDATOR_PATH NAMES glslangValidator)
     if(GLS_LANG_VALIDATOR_PATH STREQUAL "GLS_LANG_VALIDATOR_PATH-NOTFOUND")
          message(FATAL_ERROR "glslangValidator not found.")
          return()
     endif()

     cmake_parse_arguments(SHADER_COMPILE "" "INFILE;OUTFILE;NAMESPACE" "" ${ARGN})
     set(SHADER_COMPILE_INFILE_FULL "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_COMPILE_INFILE}")
     set(SHADER_COMPILE_SPV_FILE_FULL "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_COMPILE_INFILE}.spv")
     set(SHADER_COMPILE_HEADER_FILE_FULL "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_COMPILE_OUTFILE}")

     # Check if big or little endian
     include (TestBigEndian)
     TEST_BIG_ENDIAN(IS_BIG_ENDIAN)

    ## Property needs to be retrieved explicitly from globals
    get_property(VULKAN_COMPILE_SHADER_CWD GLOBAL PROPERTY VULKAN_COMPILE_SHADER_CWD_PROPERTY)
    ## The directory may not be created so we need to ensure its present 
    get_filename_component(SHADER_COMPILE_SPV_PATH ${SHADER_COMPILE_SPV_FILE_FULL} DIRECTORY)
    if(NOT EXISTS ${SHADER_COMPILE_SPV_PATH})
            get_filename_component(SHADER_COMPILE_SPV_FILENAME ${SHADER_COMPILE_SPV_FILE_FULL} NAME)
            ## Custom target names live in a *global* namespace, so the plain
            ## file name collides as soon as two examples compile a shader with
            ## the same name (e.g. gltfloading/shader/mesh.vert and
            ## geometryshader/shader/mesh.vert), which makes a from-scratch
            ## configure fail with CMP0002.  A short hash of the full output path
            ## keeps the name unique without making it long: Visual Studio turns
            ## each custom target into its own .vcxproj, and a path-length name
            ## (tried first) overflowed into MSB3501.
            string(SHA1 SHADER_COMPILE_TARGET_HASH "${SHADER_COMPILE_SPV_FILE_FULL}")
            string(SUBSTRING "${SHADER_COMPILE_TARGET_HASH}" 0 12 SHADER_COMPILE_TARGET_HASH)
            add_custom_target(build-time-make-directory-${SHADER_COMPILE_TARGET_HASH} ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_COMPILE_SPV_PATH})
    endif()

     # .comp -> .spv -> .hpp in a *single* custom command.
     ## Splitting this into two chained custom commands makes the generators
     ## check the header's up-to-date-ness before the .spv has been rewritten
     ## (MSBuild evaluates the whole CustomBuild batch first), so a shader edit
     ## needed two builds before the header - and therefore the executable -
     ## picked the new SPIR-V up.  Producing both files from one command keeps
     ## the dependency explicit.
     ## Requires a cmake script as this is the only way to call a function during
     ## compile time from cmake (ie through cmake script).
     add_custom_command(OUTPUT "${SHADER_COMPILE_SPV_FILE_FULL}"
                               "${SHADER_COMPILE_HEADER_FILE_FULL}"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_COMPILE_SPV_PATH}"
                        COMMAND "${GLS_LANG_VALIDATOR_PATH}"
                        ARGS "-V"
                             "${SHADER_COMPILE_INFILE_FULL}"
                             "-o"
                             "${SHADER_COMPILE_SPV_FILE_FULL}"
                        COMMAND ${CMAKE_COMMAND}
                        ARGS "-DINPUT_SHADER_FILE=${SHADER_COMPILE_SPV_FILE_FULL}"
                             "-DOUTPUT_HEADER_FILE=${SHADER_COMPILE_HEADER_FILE_FULL}"
                             "-DHEADER_NAMESPACE=${SHADER_COMPILE_NAMESPACE}"
                             "-DIS_BIG_ENDIAN=${IS_BIG_ENDIAN}"
                             "-P"
                             "${VULKAN_COMPILE_SHADER_CWD}/bin_file_to_header.cmake"
                        WORKING_DIRECTORY "${VULKAN_COMPILE_SHADER_CWD}"
                        COMMENT "Compiling shader '${SHADER_COMPILE_INFILE_FULL}' to '${SHADER_COMPILE_SPV_FILE_FULL}' and header '${SHADER_COMPILE_HEADER_FILE_FULL}'."
                        DEPENDS "${SHADER_COMPILE_INFILE_FULL}")
endfunction()
