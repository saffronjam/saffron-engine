# saffron_compile_shaders(<target> <shader_src_dir> <out_dir>)
#
# Compiles every *.slang in <shader_src_dir> to <out_dir>/<name>.spv with slangc (all
# [shader(...)]-tagged entry points in one module) and makes <target> depend on the results so they
# build alongside it. Also copies each .slang source next to its .spv so the runtime node-graph codegen
# can splice the übershader (mesh.slang).
#
# `lighting.slang` is special: the shared lighting half compiled once as a reusable Slang module
# (lighting.slang-module, no entry points → no .spv). mesh.slang imports it, and codegen material
# variants link it at runtime instead of recompiling the whole übershader. The module — not the source
# — is shipped to <out_dir> so a variant's `import lighting` resolves to the precompiled module.
function(saffron_compile_shaders TARGET SHADER_DIR OUT_DIR)
    file(GLOB shader_sources CONFIGURE_DEPENDS "${SHADER_DIR}/*.slang")

    set(outputs)
    set(lighting_src "${SHADER_DIR}/lighting.slang")
    set(lighting_module "${OUT_DIR}/lighting.slang-module")
    add_custom_command(
        OUTPUT ${lighting_module}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}
        COMMAND ${SAFFRON_SLANGC} ${lighting_src} -emit-ir -o ${lighting_module}
        DEPENDS ${lighting_src}
        COMMENT "slangc lighting.slang -> lighting.slang-module (shared lighting)"
        VERBATIM)
    list(APPEND outputs ${lighting_module})

    foreach(shader ${shader_sources})
        get_filename_component(name ${shader} NAME_WE)
        if(name STREQUAL "lighting")
            continue()  # compiled above as a module, not a .spv entry-point shader
        endif()
        set(out "${OUT_DIR}/${name}.spv")
        set(src_copy "${OUT_DIR}/${name}.slang")
        add_custom_command(
            OUTPUT ${out} ${src_copy}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}
            COMMAND ${SAFFRON_SLANGC} ${shader}
                    -profile glsl_450 -target spirv -emit-spirv-directly
                    -fvk-use-entrypoint-name -matrix-layout-column-major
                    -I ${SHADER_DIR}
                    -o ${out}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${shader} ${src_copy}
            DEPENDS ${shader} ${lighting_src}
            COMMENT "slangc ${name}.slang -> ${name}.spv (+ source copy)"
            VERBATIM)
        list(APPEND outputs ${out} ${src_copy})
    endforeach()

    add_custom_target(${TARGET}_shaders DEPENDS ${outputs})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
