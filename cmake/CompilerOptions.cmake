function(auralite_apply_compiler_options target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX /permissive- /EHsc /utf-8 /sdl /guard:cf
            $<$<CONFIG:Debug>:/RTC1>
            $<$<CONFIG:Release>:/GL /Gy>
        )
        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        )
        target_link_options(${target} PRIVATE
            /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT
            $<$<CONFIG:Release>:/LTCG>
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Werror -Wpedantic
        )
    endif()
endfunction()
