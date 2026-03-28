set_project ( "Testing" )
set_version ( "1.0.0"   )
set_xmakever( "2.8.0"   )

-- Generate compile_commands.json ----------------------------------------------------
rule( "vscode.compile_commands" )
    after_config( function( target )
        import( "core.base.task" ).run( "project", {
            kind      = "compile_commands",
            outputdir = ".vscode"
        })
    end )
rule_end()

add_requires( "ftxui       v6.1.9" )
add_requires( "sqlite3     3.51.0" )
add_requires( "capstone    5.0.6 ", { configs = {
    arm  = true , aarch64 = true , x86    = true,
    mips = false, ppc     = false, sparc  = false,
    sysz = false, xcore   = false
}})
add_requires( "keystone    master ", { configs = {
    arm = true, arm64 = true, x86 = true,

    build    = true,
    system   = false,
    shared   = false,
    cxflags  = "-w",
    cxxflags = {
        "-std=c++11",
        "-Wno-pedantic",
        "-Wno-unused-parameter",
        "-Wno-old-style-cast",
        "-include",
        "cstdint"
    }
}})

add_rules( "vscode.compile_commands"    )

-- MAIN TARGET -----------------------------------------------------------------------
target( "main" )
    set_default   ( true     )
    set_languages ( "c++17"  )
    set_kind      ( "binary" )
    set_filename  ( "exec"   )

    set_symbols ( "none" )
    set_optimize( "none" )
    set_warnings( "none" )

    add_files( "main.cpp", "src/**/*.cpp")
    add_files( "libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp" )

    add_includedirs( "include" )
    add_includedirs( "libs/SourceCode/json" )
    add_includedirs( "libs/SourceCode/cxxopts" )
    add_includedirs( "libs/SourceCode/DistanciaLevenshtein" )

    add_packages(
        "ftxui"      ,
        "sqlite3"    ,
        "capstone"   ,
        "openssl"
    )
    add_packages( "keystone", { public = false })

    add_defines( "__STDC_LIMIT_MACROS", "__STDC_CONSTANT_MACROS" )

    if is_plat("windows") then
        add_syslinks("ws2_32")
    end

    on_config( function( target )
        import( "core.project.config" )

        local triple  = import( "xmake.triple"       )
        local flags   = import( "xmake.config_flags" )

        local info = triple.get( target )
        if info.toolchain ~= "gcc" and info.toolchain ~= "clang" then
            cprint( "${red}This project only supports GCC and Clang toolchains, but " .. toolchain .. " is being used." )
            os.exit( 1 )
        end

        flags.apply( target, info )

        if config.get("mode") == "debug" then
          target:add("defines", "MODE_DEBUG_ENABLE")
        end

        triple.print_info( target, info )
    end )

    before_run( function( target )
        cprint( "${green}[Running: " .. target:targetfile() .. "]" )
    end )
target_end()

