# -----------------------------------------------------------------------------
# Initial setup for build system
# -----------------------------------------------------------------------------
macro( initial_setup )
    set( CMAKE_CXX_STANDARD 17 )
    set( CMAKE_CXX_STANDARD_REQUIRED True )

    set( THIRDPARTY_SOURCE_DIR "${ENGINE_SOURCE_DIR}/thirdparty" CACHE PATH "Thirdparty source directory" )
    set( BUILD_OUTPUT_DIR "game" CACHE PATH "Build output directory" )

    # VERSIONINFO on the shipped PEs tracks the player-facing label in
    # public/tier0/basetypes.h. Parsed there so the two cannot drift.
    file( READ "${ENGINE_SOURCE_DIR}/public/tier0/basetypes.h" _r5_basetypes )
    string( REGEX MATCH "SDK_DISPLAY_VERSION[ 	]+\"([0-9]+)\.([0-9]+)\.([0-9]+)" _r5_ver_match "${_r5_basetypes}" )
    if( _r5_ver_match )
        set( R5SDK_VER_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0" )
        set( R5SDK_VER_DOT "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0" )
    else()
        set( R5SDK_VER_COMMA "0,0,0,0" )
        set( R5SDK_VER_DOT "0.0.0.0" )
    endif()
    unset( _r5_basetypes )
    unset( _r5_ver_match )
    message( STATUS "PE version: ${R5SDK_VER_DOT}" )

    set( R5SDK_SIGN_THUMBPRINT "$ENV{R5SDK_SIGN_THUMBPRINT}" CACHE STRING "SHA1 thumbprint of the OV/EV Authenticode cert; empty skips signing" )
    set( R5SDK_SIGN_TIMESTAMP_URL "http://timestamp.digicert.com" CACHE STRING "RFC3161 timestamp URL" )

    if( NOT R5SDK_SIGNTOOL )
        file( GLOB _r5_signtools "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/signtool.exe" )
        if( _r5_signtools )
            list( SORT _r5_signtools )
            list( GET _r5_signtools -1 _r5_signtool_latest )
            set( R5SDK_SIGNTOOL "${_r5_signtool_latest}" CACHE FILEPATH "signtool.exe for Authenticode" )
        else()
            find_program( R5SDK_SIGNTOOL signtool )
        endif()
        unset( _r5_signtools )
        unset( _r5_signtool_latest )
    endif()

    if( R5SDK_SIGN_THUMBPRINT )
        if( NOT R5SDK_SIGNTOOL )
            message( FATAL_ERROR "R5SDK_SIGN_THUMBPRINT is set but signtool.exe was not found" )
        endif()
        message( STATUS "Authenticode: thumbprint ${R5SDK_SIGN_THUMBPRINT} via ${R5SDK_SIGNTOOL}" )
    else()
        message( STATUS "Authenticode: skipped (set R5SDK_SIGN_THUMBPRINT to an OV/EV cert SHA1)" )
    endif()

    set( GLOBAL_PCH
        "${ENGINE_SOURCE_DIR}/core/stdafx.h"
    ) # Global precompiled header shared among all libraries

    set_property( GLOBAL PROPERTY USE_FOLDERS ON ) # Use filters
endmacro()

# -----------------------------------------------------------------------------
# Set global configuration types
# -----------------------------------------------------------------------------
macro( setup_build_configurations )
    set( CMAKE_CONFIGURATION_TYPES "Debug;Profile;Release" CACHE STRING "" FORCE )
endmacro()
