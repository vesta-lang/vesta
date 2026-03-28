import( "core.project.config" )

local function reset_flags(target)
    target:set( "cflags",   {} )
    target:set( "cxxflags", {} )
    target:set( "cxflags",  {} )
    target:set( "ldflags",  {} )
end


local function is_clang(info)
    return info.toolchain:find("clang") ~= nil
end

local function is_gcc(info)
    return info.toolchain:find("gcc") ~= nil
end

local function is_release(target)
    return config.get("mode") == "release"
end


local function apply_common( target, info )
    if is_release( target ) then
        target:add( "cxflags",
            "-fvisibility=hidden",
            "-fvisibility-inlines-hidden",
            { force = true })
        return
    end

    target:add( "cxflags",
        "-pipe",
        "-O0",
        "-fdiagnostics-color=always",
        "-masm=intel",
        "-fdiagnostics-color=always",
        { force = true })
end


local function apply_warnings( target, info )
    if not is_mode("debug") then
        return
    end

    -- COMMON (C + C++) ---------------------------------------------------
    target:add("cxflags",
        "-Wall",
        "-Wextra",
        "-Wpedantic",

        "-Wconversion",
        "-Wsign-conversion",
        "-Wshadow",
        "-Wformat=2",
        "-Wundef",
        "-Wnull-dereference",
        "-Wdouble-promotion",
        "-Wimplicit-fallthrough",

        "-Wcast-align",
        "-Wcast-qual",

        "-Wduplicated-cond",
        "-Wduplicated-branches",
        "-Wlogical-op",

        "-Wmissing-declarations",
        "-Wmissing-prototypes",
        "-Wstrict-prototypes",

        "-Wwrite-strings",
        "-Wvla",

        "-Werror",
        { force = true })

    -- C ONLY ------------------------------------------------------------
    target:add("cflags",
        "-Wbad-function-cast",
        "-Wnested-externs",
        "-Wjump-misses-init",
        { force = true })

    -- C++ ONLY ----------------------------------------------------------
    target:add("cxxflags",
        "-Wnon-virtual-dtor",
        "-Wdelete-non-virtual-dtor",
        "-Woverloaded-virtual",

        "-Wold-style-cast",
        "-Wuseless-cast",
        "-Wzero-as-null-pointer-constant",

        "-Wredundant-move",
        "-Wpessimizing-move",
        "-Wdeprecated-copy",
        "-Wdeprecated-copy-dtor",

        "-Wreorder",
        "-Wclass-memaccess",
        "-Waligned-new",
        "-Wplacement-new=2",

        "-Wcatch-value",
        "-Wextra-semi",

        { force = true })

    -- GCC extras --------------------------------------------------------
    if info.toolchain:find("gcc") then
        target:add("cxflags",
            "-Wrestrict",
            "-Wduplicated-cond",
            "-Wduplicated-branches",
            { force = true })

        target:add("cxxflags",
            "-Wnoexcept",
            "-Wnoexcept-type",
            { force = true })
    end

    -- Clang extras -------------------------------------------------------
    if is_clang( info ) then
        target:add("cxxflags",
            "-Wcomma",
            "-Wextra-tokens",
            "-Wrange-loop-analysis",
            "-Wself-move",
            "-Winconsistent-missing-destructor-override",
            { force = true })
    end
end


local function apply_arch_tuning( target, info )

    if is_release( target ) then
        target:add("cxflags",
            "-march=x86-64-v2",
            "-mtune=generic",
            { force = true })
    end

    local m = ( info.bits == "64" ) and "-m64" or "-m32"
    target:add( "cxflags", m, { force = true })
    target:add( "ldflags", m, { force = true })

    if bits == "64" and info.is_msvc then
        target:add( "cxflags", "x86_64-pc-windows-msvc", { force = true }) 
    end
end


local function apply_build_mode( target, info )

    local mode = config.get( "mode" ) or "debug"

    -- DEFINES COMUNES (libstdc++ runtime checks) ----------------------
    target:add("defines",
        "_GLIBCXX_ASSERTIONS",
        { force = true })

    -- WINDOWS (clang + msvc abi o mingw) ------------------------------
    if info.os:find("windows") or info.is_msvc then
        target:add("defines",
            "WIN32_LEAN_AND_MEAN=1",
            "NOMINMAX=1",
            "UNICODE=1",
            "_UNICODE=1",
            "_DLL",
            "_MT",
            { force = true })
    end

    -- DEBUG ----------------------------------------------------------
    if not is_release( target ) then

        target:add("defines",
            "DEBUG=1",
            { force = true })

        target:add("cxflags",
            "-O0",
            "-g3",
            "-fno-inline",
            "-fno-omit-frame-pointer",
            "-fno-optimize-sibling-calls",
            "-fexceptions",
            { force = true })

    -- RELEASE -------------------------------------------------------
    elseif mode == "release" then

        target:add("defines",
            "_FORTIFY_SOURCE=2",
            "NDEBUG=1",
            { force = true })

        target:add("cxflags",
            "-O2",
            "-fomit-frame-pointer",
            "-fdata-sections",
            "-ffunction-sections",
            "-fvisibility=hidden",
            "-fvisibility-inlines-hidden",
            "-fstack-protector-strong",
            { force = true })

        -- LTO
        if info.is_msvc then
            -- Windows COFF → obligatorio lld
            target:add("cxflags", "-flto=thin", { force = true })

            -- Flags equivalentes a /OPT:REF /OPT:ICF
            target:add("ldflags",
                "-flto=thin",
                "-fuse-ld=lld",
                "-Wl,/OPT:REF",
                "-Wl,/OPT:ICF",
                "-Wl,/INCREMENTAL:NO",
                { force = true })

        else
            -- Linux ELF
            target:add("cxflags", "-flto=thin", { force = true })
            target:add("ldflags", "-flto=thin", { force = true })

            target:add("ldflags",
                "-Wl,--gc-sections",
                "-Wl,-z,relro",
                "-Wl,-z,now",
                "-Wl,-z,noexecstack",
                "-Wl,-z,defs",
                { force = true })
        end
    end
end


local function apply_linker( target, info )

    -- COFF (clang + MSVC ABI → lld-link)
    if info.is_msvc then

        if is_release( target ) then
            target:add("ldflags",
                "-Wl,/OPT:REF",
                "-Wl,/OPT:ICF",
                "-Wl,/LTCG",
                "-Wl,/DYNAMICBASE",
                "-Wl,/NXCOMPAT",
                "-Wl,/HIGHENTROPYVA",
                {force = true})
        else
            target:add("ldflags",
                "-Wl,/DEBUG",
                "-Wl,/INCREMENTAL",
                {force = true})
        end

    -- ELF o GNU PE
    else

        if is_release( target ) then
            target:add("ldflags",
                "-Wl,--gc-sections",
                "-Wl,--exclude-libs,ALL",
                "-Wl,--strip-all",
                {force = true})
        else
            target:add("ldflags",
                "-Wl,-z,relro",
                "-Wl,-z,now",
                "-Wl,-z,noexecstack",
                "-Wl,-z,defs",
                {force = true})
        end
    end
end


function apply( target, info )

    reset_flags( target )

    apply_common     ( target, info )
    apply_warnings   ( target, info )
    apply_build_mode ( target, info )
    apply_arch_tuning( target, info )
    apply_linker     ( target, info )
end
